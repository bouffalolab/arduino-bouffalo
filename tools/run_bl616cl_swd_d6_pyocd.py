#!/usr/bin/env python3
"""Run the D6 RA4M1 SRAM write/readback gate through Windows pyOCD."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

from read_bl616cl_windows_telemetry import (
    discover_windows_bus_id,
    reattach_bridge,
    resolve_wsl_usbip_host,
)
from uno_r4_usb_identity import resolve_paired_bl616cl_port, tty_properties


ROOT_DIR = Path(__file__).resolve().parents[1]
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
    parser.add_argument("--wsl-distribution", default="Ubuntu-22.04")
    parser.add_argument("--wsl-usbip-host", default="auto")
    parser.add_argument("--scratch-address", required=True, type=lambda value: int(value, 0))
    parser.add_argument("--frequency-hz", type=int, default=100000)
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


def expected_values(cycle: int, pattern: int) -> list[int]:
    return [
        (pattern ^ (cycle * 0x01010101) ^ index) & 0xFFFFFFFF
        for index in range(WORD_COUNT)
    ]


def parse_readbacks(output: str, address: int) -> list[list[int]]:
    """Extract every complete 64-byte pyOCD hex dump for the scratch buffer."""
    lines = output.splitlines()
    readbacks: list[list[int]] = []
    for start in range(len(lines)):
        first = re.match(rf"(?i)^{address:08x}:\s+(.+)$", lines[start].strip())
        if first is None:
            continue
        words: list[int] = []
        for line in lines[start : start + 4]:
            match = re.match(r"(?i)^([0-9a-f]{8}):\s+(.+)$", line.strip())
            if match is None:
                break
            line_address = int(match.group(1), 16)
            if line_address != address + len(words) * 4:
                break
            values = re.findall(r"(?i)\b[0-9a-f]{8}\b", match.group(2))
            words.extend(int(value, 16) for value in values)
        if len(words) == WORD_COUNT:
            readbacks.append(words)
    return readbacks


def run_windows_pyocd(
    bus_id: str,
    address: int,
    frequency_hz: int,
    cycles: int,
) -> str:
    commands = ["halt", f"read32 0x{address:08X} {WORD_COUNT * 4}"]
    expected: list[list[int]] = []
    for cycle in range(cycles):
        for pattern in PATTERNS:
            values = expected_values(cycle, pattern)
            expected.append(values)
            serialized_values = " ".join(f"0x{value:08X}" for value in values)
            commands.append(f"write32 0x{address:08X} {serialized_values}")
            commands.append(f"read32 0x{address:08X} {WORD_COUNT * 4}")
    commands += ["continue", "reset", "status"]
    command_text = " ".join(
        f"-c '{command.replace(chr(39), chr(39) + chr(92) + chr(39) + chr(39))}'"
        for command in commands
    )
    script = (
        "$ErrorActionPreference = 'Stop'; "
        f"usbipd wsl detach --busid {bus_id}; "
        "Start-Sleep -Milliseconds 1000; "
        "python -m pyocd commander --target cortex_m "
        "--uid BL616CL-PROVISIONAL "
        f"--frequency {frequency_hz} --no-wait {command_text}; "
        "if ($LASTEXITCODE -ne 0) { "
        "throw \"Windows pyOCD D6 commander failed with exit code $LASTEXITCODE\" "
        "}"
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", script],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=120.0,
    )
    output = result.stdout + result.stderr
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        raise RuntimeError(
            f"Windows pyOCD D6 session failed with exit code {result.returncode}"
        )
    if "Successfully halted device" not in output:
        raise RuntimeError("Windows pyOCD D6 did not halt RA4M1")
    if "Core 0 (None):  Running" not in output:
        raise RuntimeError("Windows pyOCD D6 did not resume/reset RA4M1")
    readbacks = parse_readbacks(output, address)
    if readbacks[1:] != expected:
        raise RuntimeError(
            "Windows pyOCD D6 readback sequence mismatch: "
            f"expected {len(expected)} groups after seed read, "
            f"observed {len(readbacks) - 1}"
        )
    print("BL616CL_RA4_SWD_D6_PYOCD_PATTERN_PASS")
    return output


def main() -> int:
    args = parse_arguments()
    if (
        args.scratch_address % 4
        or args.scratch_address < 0x20000000
        or args.scratch_address + WORD_COUNT * 4 > 0x20008000
    ):
        raise RuntimeError("scratch address must be a word-aligned RA4M1 SRAM range")
    if args.frequency_hz <= 0 or args.cycles <= 0:
        raise RuntimeError("frequency and cycles must be positive")

    port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier(port)
    bus_id = discover_windows_bus_id()
    host = resolve_wsl_usbip_host(args.wsl_usbip_host)
    print(f"BL616CL_FINAL_CARRIER_PORT={port}")
    print(f"RA4_SWD_SCRATCH_ADDRESS=0x{args.scratch_address:08X}")
    print("Target: RA4M1 SRAM scratch write/readback only; no Flash is written.")
    try:
        run_windows_pyocd(
            bus_id,
            args.scratch_address,
            args.frequency_hz,
            args.cycles,
        )
    finally:
        reattach_bridge(
            args.wsl_distribution,
            host,
            bus_id,
            port,
            20.0,
        )
    require_final_carrier(port)
    print("BL616CL_RA4_SWD_D6_PYOCD_SESSION_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
