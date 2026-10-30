#!/usr/bin/env python3
"""Build the standalone RA4M1 SRAM FLASH_LP loader."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
DEFAULT_TOOLCHAIN = (
    Path.home()
    / ".arduino15/packages/arduino/tools/arm-none-eabi-gcc/7-2017q4/bin"
)


def run(command: list[str]) -> None:
    print("COMMAND=" + " ".join(str(item) for item in command))
    subprocess.run(command, check=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--toolchain-bin", type=Path, default=DEFAULT_TOOLCHAIN)
    parser.add_argument(
        "--output-dir",
        type=Path,
        default=ROOT / "build/ra4-swd-flash-loader",
    )
    args = parser.parse_args()

    gcc = args.toolchain_bin / "arm-none-eabi-gcc"
    objcopy = args.toolchain_bin / "arm-none-eabi-objcopy"
    size = args.toolchain_bin / "arm-none-eabi-size"
    for tool in (gcc, objcopy, size):
        if not tool.is_file() or not os.access(tool, os.X_OK):
            raise RuntimeError(f"ARM GCC tool is unavailable: {tool}")

    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    elf = output / "ra4_swd_flash_loader.elf"
    binary = output / "ra4_swd_flash_loader.bin"
    map_file = output / "ra4_swd_flash_loader.map"
    object_file = output / "ra4_swd_flash_loader.o"

    common = [
        "-mcpu=cortex-m4",
        "-mthumb",
        "-mfloat-abi=soft",
        "-ffreestanding",
        "-fno-builtin",
        "-fno-stack-protector",
        "-fno-unwind-tables",
        "-fno-asynchronous-unwind-tables",
        "-fdata-sections",
        "-ffunction-sections",
        "-Os",
        "-Wall",
        "-Wextra",
        "-Werror",
    ]
    run([str(gcc), *common, "-c", str(HERE / "ra4_swd_flash_loader.c"), "-o", str(object_file)])
    run(
        [
            str(gcc),
            *common,
            "-nostdlib",
            "-nostartfiles",
            "-nodefaultlibs",
            "-Wl,--gc-sections",
            f"-Wl,-T,{HERE / 'ra4_swd_flash_loader.ld'}",
            f"-Wl,-Map,{map_file}",
            str(object_file),
            "-o",
            str(elf),
        ]
    )
    run([str(objcopy), "-O", "binary", str(elf), str(binary)])
    run([str(size), str(elf)])
    if binary.stat().st_size > 0x2000:
        raise RuntimeError(
            f"loader is {binary.stat().st_size} bytes, exceeding 8 KiB SRAM region"
        )
    print(f"RA4_SWD_FLASH_LOADER_ELF={elf}")
    print(f"RA4_SWD_FLASH_LOADER_BIN={binary}")
    print(f"RA4_SWD_FLASH_LOADER_BYTES={binary.stat().st_size}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
