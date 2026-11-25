#!/usr/bin/env python3
"""
Parse-M1A-Probe.py - Decoder & Diff Analyzer for MacBookPro13,3 Milestone 1A UEFI Logs

Parses probe_*.txt files produced by mbp133_m1a_probe.efi, decodes Polaris 11 SMU7,
PCI/ECAM, Intel IGPU/OpRegion, Apple Indexed GMUX, and VT-d IOMMU snapshots,
computes exact diffs between Normal and AppleSetOs (10.15 / 10.9) snapshots,
and writes diag/m1a/M1A_ANALYSIS.md.

SPDX-License-Identifier: MIT
"""

import argparse
import os
import re
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple


SNAP_RE = re.compile(
    r"^=+\r?\nSNAPSHOT:\s*(.+?)\r?\n=+\r?\n(.*?)(?=^=+\r?\nSNAPSHOT:|^LOG_BYTES=|\Z)",
    re.M | re.S,
)

PCI_BLOCK_RE = re.compile(
    r"^\s*\[(PCI\s+[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}\.[0-9A-Fa-f]\s+\S+|ECAM\s+[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}\.[0-9A-Fa-f]\s+\S+)\s+[^\]]+\]\r?\n(.*?)(?=^\s*\[|\Z)",
    re.M | re.S,
)


def parse_pci_bytes(block_text: str) -> Dict[int, int]:
    """Parse hex dword rows (e.g. '00: 8086191B ...') into {byte_offset: dword}."""
    out: Dict[int, int] = {}
    for line in block_text.splitlines():
        m = re.match(r"^\s*([0-9A-Fa-f]{2,3}):\s+([0-9A-Fa-f\s]+)$", line.strip())
        if not m:
            continue
        base = int(m.group(1), 16)
        words = m.group(2).split()
        for idx, w in enumerate(words):
            if len(w) == 8:
                out[base + idx * 4] = int(w, 16)
    return out


def extract_key_fields(snap_text: str) -> Dict[str, str]:
    """Extract normalized key telemetry fields from a single snapshot section."""
    fields: Dict[str, str] = {}

    patterns = [
        ("AppleSetOs", r"AppleSetOs:\s*(.+)"),
        ("gpu-power-prefs", r"gpu-power-prefs:\s*(.+)"),
        ("HostBridge_DEVEN", r"GGC=(0x[0-9A-Fa-f]+)\s+DEVEN=(0x[0-9A-Fa-f]+\s*\([^)]+\))"),
        ("PEG0_CMD_MEM", r"\[00:01\.0 PEG0 Decoded\]\r?\n\s*(.+)"),
        ("PEG0_ECAM", r"ECAM 0x248=(.+)"),
        ("IGPU_PCI", r"\[00:02\.0 Intel HD 530 Decoded\]\r?\n\s*(.+)"),
        ("IGPU_OpRegion", r"(OpRegion sig=.+)"),
        ("IGPU_MMIO", r"(IGPU MMIO:.+)"),
        ("AMD_PCI", r"\[AMD Polaris 11 \(01:00\.0\)[^\]]*\]\r?\n\s*(.+)"),
        ("AMD_PMCSR", r"(PMCSR=0x[0-9A-Fa-f]+\s*\([^)]+\))"),
        ("AMD_PCIe_Link", r"(DevCtl=0x[0-9A-Fa-f]+\s+DevSta=0x[0-9A-Fa-f]+\s+LnkCtl=0x[0-9A-Fa-f]+\s+LnkSta=0x[0-9A-Fa-f]+\s*\([^)]+\))"),
        ("AMD_BAR5_1", r"(BAR5 Direct:\s*CONFIG_MEMSIZE=.+)"),
        ("AMD_BAR5_2", r"(BAR5 Direct:\s*GRBM_STATUS=.+)"),
        ("AMD_SCRATCH", r"(BIOS_SCRATCH\[0\.\.7\]:\s*.+)"),
        ("SMC_RESET_CNTL", r"ixSMC_SYSCON_RESET_CNTL\s*=\s*(.+)"),
        ("SMC_CLOCK_CNTL_0", r"ixSMC_SYSCON_CLOCK_CNTL_0\s*=\s*(.+)"),
        ("SMC_MISC_CNTL", r"ixSMC_SYSCON_MISC_CNTL\s*=\s*(.+)"),
        ("SMC_PC_C", r"ixSMC_PC_C \(t0 / \+5ms\)\s*=\s*(.+)"),
        ("RCU_UC_EVENTS", r"ixRCU_UC_EVENTS\s*=\s*(.+)"),
        ("SMU_STATUS", r"ixSMU_STATUS\s*=\s*(.+)"),
        ("SMU_FIRMWARE", r"ixSMU_FIRMWARE\s*=\s*(.+)"),
        ("FIRMWARE_FLAGS", r"ixFIRMWARE_FLAGS\s*=\s*(.+)"),
        ("SMU_EFUSE_49", r"ixSMU_EFUSE_49\s*=\s*(.+)"),
        ("GMUX_Probe", r"(Probe sig=.+)"),
        ("GMUX_Ver", r"(VERSION\(0x04\)=.+)"),
        ("GMUX_Disp_0x10", r"Port 0x10 \(SWITCH_DISPLAY\):\s*(.+)"),
        ("GMUX_Pwr_0x50", r"Port 0x50 \(DISCRETE_POWER\):\s*(.+)"),
        ("IOMMU_0", r"(DRHD@0xFED90000.+)"),
        ("IOMMU_1", r"(DRHD@0xFED91000.+)"),
        ("VFCT", r"(VFCT VBIOS.+)"),
    ]

    for key, pat in patterns:
        m = re.search(pat, snap_text, re.M)
        if m:
            if key == "HostBridge_DEVEN":
                fields[key] = f"GGC={m.group(1)} DEVEN={m.group(2)}"
            else:
                fields[key] = m.group(1).strip()

    for m in re.finditer(r"SKIP\s+(BAR5|SMC_IND):\s*(.+)", snap_text):
        fields[f"SKIP_{m.group(1)}"] = m.group(2).strip()

    sram_m = re.search(r"SMC SRAM 0x20000\.\.0x2003C:\r?\n((?:\s+0x200[0-9A-Fa-f]+:.*\r?\n)+)", snap_text)
    if sram_m:
        fields["SMC_SRAM_HDR"] = " | ".join(line.strip() for line in sram_m.group(1).strip().splitlines())

    return fields


def parse_probe_file(path: Path) -> Dict[str, object]:
    text = path.read_text(encoding="utf-8", errors="replace")
    mode_m = re.search(r"^MODE_SETOS=(.+)$", text, re.M)
    mode_setos = mode_m.group(1).strip() if mode_m else "UNKNOWN"

    snapshots: List[Tuple[str, str, Dict[str, str], Dict[str, Dict[int, int]]]] = []
    for m in SNAP_RE.finditer(text):
        tag = m.group(1).strip()
        body = m.group(2)
        fields = extract_key_fields(body)
        pci_blocks: Dict[str, Dict[int, int]] = {}
        for pm in PCI_BLOCK_RE.finditer(body):
            bname = re.sub(r"\s+", " ", pm.group(1).strip())
            parsed = parse_pci_bytes(pm.group(2))
            if parsed:
                pci_blocks[bname] = parsed
        snapshots.append((tag, body, fields, pci_blocks))

    return {
        "file": path.name,
        "path": str(path),
        "mode_setos": mode_setos,
        "snapshots": snapshots,
    }


def diff_fields(a_name: str, a: Dict[str, str], b_name: str, b: Dict[str, str]) -> List[str]:
    lines: List[str] = []
    keys = list(dict.fromkeys(list(a.keys()) + list(b.keys())))
    for k in keys:
        va = a.get(k, "<absent>")
        vb = b.get(k, "<absent>")
        if va != vb:
            lines.append(f"- **`{k}`**\n  - `{a_name}`: `{va}`\n  - `{b_name}`: `{vb}`")
    return lines


def diff_pci_blocks(
    a_name: str,
    a_blocks: Dict[str, Dict[int, int]],
    b_name: str,
    b_blocks: Dict[str, Dict[int, int]],
) -> List[str]:
    lines: List[str] = []
    names = list(dict.fromkeys(list(a_blocks.keys()) + list(b_blocks.keys())))
    for name in names:
        da = a_blocks.get(name, {})
        db = b_blocks.get(name, {})
        if not da or not db:
            lines.append(f"- `{name}`: present in `{a_name if da else b_name}` only")
            continue
        offs = sorted(set(da.keys()) | set(db.keys()))
        diffs = []
        for off in offs:
            va = da.get(off)
            vb = db.get(off)
            if va != vb:
                sa = f"0x{va:08X}" if va is not None else "----"
                sb = f"0x{vb:08X}" if vb is not None else "----"
                diffs.append(f"+0x{off:03X}: `{sa}` -> `{sb}`")
        if diffs:
            lines.append(f"- **`{name}`** ({len(diffs)} dword diffs): " + ", ".join(diffs[:24]))
            if len(diffs) > 24:
                lines.append(f"  - ... and {len(diffs) - 24} more offsets")
    return lines


def build_report(parsed_files: List[Dict[str, object]]) -> str:
    out: List[str] = [
        "# Milestone 1A UEFI Probe Analysis (`MacBookPro13,3`)",
        "",
        f"Parsed {len(parsed_files)} UEFI log file(s).",
        "",
    ]

    flat_snaps: List[Tuple[str, Dict[str, str], Dict[str, Dict[int, int]]]] = []

    for pf in parsed_files:
        out.append(f"## File: `{pf['file']}` (`MODE_SETOS={pf['mode_setos']}`)")
        out.append("")
        snaps = pf["snapshots"]
        if not snaps:
            out.append("> [!WARNING]\n> No `SNAPSHOT:` sections found in this file.")
            out.append("")
            continue
        for tag, _body, fields, pci_blocks in snaps:
            snap_id = f"{pf['file']}:{tag}"
            flat_snaps.append((snap_id, fields, pci_blocks))
            out.append(f"### Snapshot `{tag}`")
            out.append("")
            out.append("| Telemetry Item | Value |")
            out.append("| :--- | :--- |")
            for k, v in fields.items():
                v_clean = v.replace("|", "\\|")
                out.append(f"| `{k}` | `{v_clean}` |")
            out.append("")

    if len(flat_snaps) >= 2:
        out.append("## Snapshot Diffs")
        out.append("")

        # Prefer comparing normal/BEFORE_SETOS against AFTER_SETOS_* snapshots
        baseline = None
        for item in flat_snaps:
            if "probe_normal" in item[0] or "probe_only_nrm" in item[0]:
                baseline = item
                break
        if baseline is None:
            baseline = flat_snaps[0]

        base_id, base_f, base_p = baseline
        for cmp_id, cmp_f, cmp_p in flat_snaps:
            if cmp_id == base_id:
                continue
            out.append(f"### Diff: `{base_id}` vs `{cmp_id}`")
            out.append("")
            fdiffs = diff_fields(base_id, base_f, cmp_id, cmp_f)
            pdiffs = diff_pci_blocks(base_id, base_p, cmp_id, cmp_p)
            if not fdiffs and not pdiffs:
                out.append("- **100% IDENTICAL** across all decoded telemetry fields and PCI/ECAM dwords.")
            else:
                if fdiffs:
                    out.append("#### Telemetry Field Changes")
                    out.extend(fdiffs)
                    out.append("")
                if pdiffs:
                    out.append("#### Raw PCI / ECAM Dword Changes")
                    out.extend(pdiffs)
                    out.append("")

    return "\n".join(out) + "\n"


def main() -> int:
    ap = argparse.ArgumentParser(description="Analyze MacBookPro13,3 Milestone 1A UEFI probe logs.")
    ap.add_argument("--input-dir", default=None, help="Directory containing probe_*.txt logs")
    ap.add_argument("--output", default=None, help="Output Markdown analysis file")
    args = ap.parse_args()

    repo_root = Path(__file__).resolve().parent.parent
    in_dir = Path(args.input_dir) if args.input_dir else (repo_root / "diag" / "m1a" / "uefi")
    out_file = Path(args.output) if args.output else (repo_root / "diag" / "m1a" / "M1A_ANALYSIS.md")

    if not in_dir.exists():
        print(f"[INFO] Input directory does not exist yet: {in_dir}")
        return 0

    logs = sorted(in_dir.glob("probe_*.txt"))
    if not logs:
        print(f"[INFO] No probe_*.txt files found in {in_dir}")
        return 0

    parsed = [parse_probe_file(p) for p in logs]
    report = build_report(parsed)
    out_file.parent.mkdir(parents=True, exist_ok=True)
    out_file.write_text(report, encoding="utf-8")
    print(f"[OK] Wrote analysis for {len(logs)} probe log(s) -> {out_file}")

    for pf in parsed:
        print(f"  - {pf['file']} (MODE_SETOS={pf['mode_setos']}): {len(pf['snapshots'])} snapshot(s)")
        for tag, _b, fields, _p in pf["snapshots"]:
            smu_sts = fields.get("SMU_STATUS", "n/a")
            smc_pc = fields.get("SMC_PC_C", fields.get("SKIP_SMC_IND", "n/a"))
            deven = fields.get("HostBridge_DEVEN", "n/a")
            print(f"      [{tag}] {deven} | SMU_STATUS={smu_sts} | PC={smc_pc}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
