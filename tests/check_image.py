#!/usr/bin/env python3
"""检查编译目录中的镜像后处理产物，上传配方直接使用这些文件。

  check_image.py <编译目录> <project_name>

未经后处理的 .bin 启动头保留链接时的占位值（CRC 为 0xDEADBEEF），ROM 不会引导；
partition.bin 与 boot2 副本由后处理生成，缺少时上传会失败。任一项不满足时退出码为 1。
"""

import struct
import sys
from pathlib import Path

HEADER_SIZE = 0x1000
PLACEHOLDER_CRC = 0xDEADBEEF


def check(build, project):
    results = []
    img = (build / (project + ".bin")).read_bytes()
    length, = struct.unpack_from("<I", img, 0x84)
    crc, = struct.unpack_from("<I", img, 0xFC)
    results.append(("固件启动头魔数 BFNP", img[:4] == b"BFNP"))
    results.append(("固件启动头已由后处理写入（长度 0x%x，CRC 0x%08x）" % (length, crc),
                    length == len(img) - HEADER_SIZE and crc != PLACEHOLDER_CRC))
    pt = build / "partition.bin"
    results.append(("partition.bin 魔数 BFPT", pt.is_file() and pt.read_bytes()[:4] == b"BFPT"))
    results.append(("boot2 副本存在", any(build.glob("boot2_*.bin"))))
    return results


def main(argv):
    if len(argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2
    results = check(Path(argv[0]), argv[1])
    for name, ok in results:
        print("%s: %s" % (name, "PASS" if ok else "FAIL"))
    return 0 if all(ok for _, ok in results) else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
