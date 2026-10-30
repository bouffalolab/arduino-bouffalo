#!/usr/bin/env python3
"""Check post-processed build images consumed directly by the upload recipe. Usage:

check_image.py <build-directory> <project_name>. An unprocessed boot header retains
linker placeholders (CRC 0xDEADBEEF) and will not boot. Post-processing must create
partition.bin; missing or invalid outputs cause exit status 1.
"""

import struct
import sys
from pathlib import Path

HEADER_SIZE = 0x1000
PLACEHOLDER_CRC = 0xDEADBEEF


def check(build, project):
    """Validate the selected artifact or fixture and raise an error on a failed invariant."""
    results = []
    img = (build / (project + ".bin")).read_bytes()
    length, = struct.unpack_from("<I", img, 0x84)
    crc, = struct.unpack_from("<I", img, 0xFC)
    results.append(("固件启动头魔数 BFNP", img[:4] == b"BFNP"))
    results.append(("固件启动头已由后处理写入（长度 0x%x，CRC 0x%08x）" % (length, crc),
                    length == len(img) - HEADER_SIZE and crc != PLACEHOLDER_CRC))
    pt = build / "partition.bin"
    results.append(("partition.bin 魔数 BFPT", pt.is_file() and pt.read_bytes()[:4] == b"BFPT"))
    return results


def main(argv):
    """Parse options and run the check image validation or packaging command."""
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    results = check(Path(argv[0]), argv[1])
    for name, ok in results:
        print("%s: %s" % (name, "PASS" if ok else "FAIL"))
    return 0 if all(ok for _, ok in results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
