#!/usr/bin/env python3
"""Check the bridge's real SRAM heap regions from its linker map."""

import argparse
import re
from pathlib import Path


MIN_OCRAM_HEAP = 44 * 1024
MIN_WRAM_HEAP = 80 * 1024
SYMBOLS = ("__HeapBase", "__HeapLimit", "_heap_wifi_size")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("map", type=Path)
    args = parser.parse_args()
    contents = args.map.read_text(encoding="utf-8", errors="replace")
    values = {}
    for symbol in SYMBOLS:
        matches = re.findall(
            rf"^\s*(0x[0-9a-fA-F]+)\s+{re.escape(symbol)}\s*=",
            contents,
            re.MULTILINE,
        )
        if len(matches) != 1:
            raise RuntimeError(f"expected one linker definition for {symbol}")
        values[symbol] = int(matches[0], 16)

    ocram_heap = values["__HeapLimit"] - values["__HeapBase"]
    wram_heap = values["_heap_wifi_size"]
    print(f"BL616CL_MEMORY_BUDGET ocram_heap={ocram_heap} wram_heap={wram_heap}")
    if ocram_heap < MIN_OCRAM_HEAP or wram_heap < MIN_WRAM_HEAP:
        raise RuntimeError(
            f"bridge SRAM heap below minimum: OCRAM >= {MIN_OCRAM_HEAP}, "
            f"WRAM >= {MIN_WRAM_HEAP}"
        )
    print("BL616CL_MEMORY_BUDGET_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
