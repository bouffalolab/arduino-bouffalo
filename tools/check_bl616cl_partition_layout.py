#!/usr/bin/env python3
"""Enforce the frozen 4 MiB bridge layout before accepting a build."""
import argparse
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]
LAYOUT = ROOT / "./tools/partitions/partition_cfg_4M_bridge_v1.toml"
EXPECTED = {
    "Boot2": (0, 0xE000, 0, 0),
    "FW": (0x10000, 0x1B0000, 0x210000, 0x168000),
    "mfg": (0x210000, 0x168000, 0, 0),
    "RA4_OTA": (0x1C0000, 0x50000, 0, 0),
    "media": (0x378000, 0x71000, 0, 0),
    "PSM": (0x3E9000, 0x8000, 0, 0),
    "KEY": (0x3F1000, 0x2000, 0, 0),
    "DATA": (0x3F3000, 0x5000, 0, 0),
    "factory": (0x3F8000, 0x8000, 0, 0),
}


def validate(path, application=None):
    sections = path.read_text().split("[[pt_entry]]")
    table = dict(re.findall(r"^\s*(address[01]|version)\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)", sections[0], re.M))
    if {k: int(v, 0) for k,v in table.items()} != {"address0": 0xE000, "address1": 0xF000, "version": 2}:
        raise RuntimeError("partition table header differs from bridge layout v1")
    entries = {}
    for section in sections[1:]:
        name = re.search(r'^name\s*=\s*"([^"]+)"',section,re.M)[1]
        fields = dict(re.findall(r"^(address[01]|size[01])\s*=\s*(0x[0-9a-fA-F]+|[0-9]+)",section,re.M))
        if name in entries:
            raise RuntimeError("duplicate partition")
        entries[name] = tuple(int(fields[k],0) for k in ("address0","size0","address1","size1"))
    if entries != EXPECTED:
        raise RuntimeError("partition map differs from frozen bridge layout v1")
    if application is not None:
        # Check the postprocessed image including its boot header, not ELF text.
        size=application.stat().st_size
        if not 0 < size <= min(entries["FW"][1], entries["FW"][3]):
            raise RuntimeError("application does not fit both firmware slots")
        print(f"BRIDGE_FW_SLOT_CAPACITY_PASS image={size} primary={entries['FW'][1]} secondary={entries['FW'][3]}")
    print("BL616CL_PARTITION_LAYOUT_V1_PASS ra4_cache=327680 ra4_application_max=245760 media=462848 psm=32768")


if __name__ == "__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--layout",type=Path,default=LAYOUT)
    p.add_argument("--application",type=Path)
    args=p.parse_args()
    validate(args.layout,args.application)
