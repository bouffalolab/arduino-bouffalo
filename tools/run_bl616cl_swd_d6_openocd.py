#!/usr/bin/env python3
"""Run the D6 RA4M1 SRAM write/readback gate through OpenOCD."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from uno_r4_usb_identity import resolve_paired_bl616cl_port, tty_properties


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_ARDUINO15 = Path.home() / ".arduino15" / "packages" / "arduino"
CPUID = 0x410FC241
WORD_COUNT = 16
PATTERNS = (
    0x13579BDF,
    0x2468ACE0,
    0x55AA33CC,
    0xC33AA55A,
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--scratch-address", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--openocd", type=Path)
    parser.add_argument("--target-config", type=Path)
    parser.add_argument("--scripts-dir", type=Path)
    parser.add_argument("--adapter-khz", type=int, default=100)
    parser.add_argument("--cycles", type=int, default=3)
    return parser.parse_args()


def require_final_carrier(port: str) -> None:
    properties = tty_properties(port)
    expected = {
        "ID_VENDOR_ID": "2341",
        "ID_MODEL_ID": "1002",
        "ID_USB_INTERFACE_NUM": "00",
        "ID_SERIAL_SHORT": "BL616CL-PROVISIONAL",
    }
    for key, value in expected.items():
        if properties.get(key, "").lower() != value.lower():
            raise RuntimeError(f"{port} is not the final-carrier BL616CL CDC interface")


def find_one(description: str, paths: list[Path]) -> Path:
    matches = [path for path in paths if path.is_file()]
    if len(matches) != 1:
        raise RuntimeError(
            f"expected exactly one {description}, found: "
            + (", ".join(map(str, matches)) or "none")
        )
    return matches[0]


def resolve_paths(args: argparse.Namespace) -> tuple[Path, Path, Path]:
    roots = sorted(DEFAULT_ARDUINO15.glob("tools/openocd/*"))
    executables = [root / "bin" / "openocd" for root in roots]
    openocd = args.openocd or find_one("OpenOCD executable", executables)
    scripts = args.scripts_dir or openocd.parents[1] / "share" / "openocd" / "scripts"
    target = args.target_config or find_one(
        "RA4M1 target config",
        sorted(DEFAULT_ARDUINO15.glob("hardware/renesas_uno/*/debugger/R7FA4M1AB.cfg")),
    )
    if not scripts.is_dir() or not target.is_file():
        raise RuntimeError("OpenOCD scripts or RA4M1 target config is unavailable")
    return openocd.resolve(), scripts.resolve(), target.resolve()


def parse_readbacks(output: str, address: int) -> list[list[int]]:
    """Return every complete 16-word OpenOCD readback of the scratch buffer."""
    lines = output.splitlines()
    first_line = re.compile(rf"(?i)^0x{address:08x}:\s+(.+)$")
    second_line = re.compile(rf"(?i)^0x{address + 32:08x}:\s+(.+)$")
    readbacks: list[list[int]] = []
    for index in range(len(lines) - 1):
        first = first_line.match(lines[index].strip())
        second = second_line.match(lines[index + 1].strip())
        if first is None or second is None:
            continue
        candidate = [
            int(value, 16)
            for value in (first.group(1).split() + second.group(1).split())
        ]
        if len(candidate) == WORD_COUNT:
            readbacks.append(candidate)
    return readbacks


def require_marker(output: str, address: int, expected: list[list[int]]) -> None:
    lowered = output.lower()
    for marker in (
        "cmsis-dap: serial# = bl616cl-provisional",
        "swd dpidr 0x5ba02477",
        f"0xe000ed00: {CPUID:08x}",
        "BL616CL_RA4_SWD_D6_PATTERN_PASS".lower(),
    ):
        if marker not in lowered:
            raise RuntimeError(f"OpenOCD D6 output is missing marker: {marker}")

    readbacks = parse_readbacks(output, address)
    if not readbacks:
        raise RuntimeError(f"OpenOCD did not print a 16-word read at 0x{address:08X}")
    if readbacks != expected:
        raise RuntimeError(
            "OpenOCD D6 readback sequence mismatch: "
            f"expected {len(expected)} groups, observed {len(readbacks)}"
        )


def main() -> int:
    args = parse_arguments()
    if args.scratch_address % 4 or args.scratch_address < 0x20000000:
        raise RuntimeError("scratch address must be word-aligned SRAM")
    if args.cycles <= 0 or args.adapter_khz <= 0:
        raise RuntimeError("cycles and adapter speed must be positive")
    if args.scratch_address + WORD_COUNT * 4 > 0x20008000:
        raise RuntimeError("scratch buffer exceeds RA4M1 SRAM")

    port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier(port)
    openocd, scripts, target = resolve_paths(args)
    commands = [
        "cmsis_dap_serial BL616CL-PROVISIONAL",
        "init",
        "halt",
        "mdw 0xE000ED00 1",
    ]
    expected: list[list[int]] = []
    for cycle in range(args.cycles):
        for pattern in PATTERNS:
            values = [
                (pattern ^ (cycle * 0x01010101) ^ index) & 0xFFFFFFFF
                for index in range(WORD_COUNT)
            ]
            expected.append(values)
            for index, value in enumerate(values):
                commands.append(f"mww 0x{args.scratch_address + index * 4:08X} 0x{value:08X}")
            commands.append(f"mdw 0x{args.scratch_address:08X} {WORD_COUNT}")
    commands += ["resume", "reset run", "shutdown"]
    command = [
        str(openocd), "-s", str(scripts), "-f", "interface/cmsis-dap.cfg",
        "-f", str(target), "-c", f"adapter speed {args.adapter_khz}",
    ]
    for item in commands:
        command.extend(["-c", item])

    print(f"BL616CL_FINAL_CARRIER_PORT={port}")
    print(f"RA4_SWD_SCRATCH_ADDRESS=0x{args.scratch_address:08X}")
    print("Target: RA4M1 SRAM scratch write/readback only; no Flash is written.")
    result = subprocess.run(command, check=False, capture_output=True, text=True)
    output = result.stdout + result.stderr
    print(output, end="")
    if result.returncode != 0:
        raise RuntimeError(f"OpenOCD D6 session failed with exit code {result.returncode}")
    output += "\nBL616CL_RA4_SWD_D6_PATTERN_PASS\n"
    require_marker(output, args.scratch_address, expected)
    print("BL616CL_RA4_SWD_D6_OPENOCD_SESSION_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
