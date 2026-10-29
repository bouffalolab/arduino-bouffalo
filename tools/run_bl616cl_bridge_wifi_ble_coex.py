#!/usr/bin/env python3
"""Flash and capture the standalone BL616CL Wi-Fi + BLE coexistence smoke."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys
import time

try:
    import serial
except ImportError as exc:
    print(f"pyserial is required: {exc}", file=sys.stderr)
    raise SystemExit(2)


EXPECTED_PROPERTIES = {
    "ID_VENDOR_ID": "42bf",
    "ID_MODEL_ID": "b210",
    "ID_SERIAL_SHORT": "FactoryAIOT_Prog_B2DDDD",
}
RESET_COMMANDS = (b"BOUFFALOLAB5555RTS0", b"BOUFFALOLAB5555RTS1")
DEFAULT_SUCCESS_PREFIX = b"BL616CL_SMOKE:COEX_STABLE_DONE cycles=3"
FAILURE_PREFIX = b"BL616CL_SMOKE:FAIL "


def port_properties(port: str) -> dict[str, str]:
    result = subprocess.run(
        ["udevadm", "info", "--query=property", f"--name={port}"],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    properties: dict[str, str] = {}
    for line in result.stdout.splitlines():
        key, separator, value = line.partition("=")
        if separator:
            properties[key] = value
    return properties


def verify_target(port: str) -> None:
    if not Path(port).is_char_device():
        raise RuntimeError(f"missing serial port: {port}")

    properties = port_properties(port)
    mismatches = [
        f"{key}={properties.get(key, '<missing>')}"
        for key, expected in EXPECTED_PROPERTIES.items()
        if properties.get(key) != expected
    ]
    if mismatches:
        raise RuntimeError(
            f"refusing to use {port}; expected dedicated BL616CL CKLink-Lite "
            f"identity, observed {', '.join(mismatches)}"
        )


def flash(
    flash_tool: Path,
    boot2: Path,
    partition: Path,
    firmware: Path,
    port: str,
) -> None:
    subprocess.run(
        [
            str(flash_tool),
            "--interface=uart",
            "--chipname=bl616cl",
            f"--port={port}",
            "--baudrate=2000000",
            "write_flash_files",
            "0x0",
            str(boot2),
            "0xE000",
            str(partition),
            "0x10000",
            str(firmware),
        ],
        check=True,
    )


def capture(port: str, timeout_seconds: float, success_prefix: bytes) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    captured = bytearray()

    with serial.Serial(port, 2000000, timeout=0.05, write_timeout=2) as link:
        link.dtr = False
        link.rts = False
        link.reset_input_buffer()
        for command in RESET_COMMANDS:
            link.write(command)
            link.flush()
            time.sleep(0.05)

        while time.monotonic() < deadline:
            waiting = link.in_waiting
            if waiting:
                captured.extend(link.read(waiting))
                if success_prefix in captured or FAILURE_PREFIX in captured:
                    break
            else:
                time.sleep(0.01)

    return bytes(captured)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM1")
    parser.add_argument("--boot2", required=True, type=Path)
    parser.add_argument("--partition", required=True, type=Path)
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--flash-tool", required=True, type=Path)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=90.0)
    parser.add_argument(
        "--success-prefix",
        default=DEFAULT_SUCCESS_PREFIX.decode("ascii"),
        help="ASCII runtime marker required for a successful capture",
    )
    args = parser.parse_args()

    for name, path in (
        ("boot2", args.boot2),
        ("partition", args.partition),
        ("firmware", args.firmware),
    ):
        if not path.is_file():
            raise RuntimeError(f"{name} not found: {path}")
    if not args.flash_tool.is_file():
        raise RuntimeError(f"FlashCube tool not found: {args.flash_tool}")
    try:
        success_prefix = args.success_prefix.encode("ascii")
    except UnicodeEncodeError as error:
        raise RuntimeError("--success-prefix must be ASCII") from error
    if not success_prefix:
        raise RuntimeError("--success-prefix must not be empty")

    verify_target(args.port)
    print(f"target: {args.port} BL616CL CKLink-Lite {EXPECTED_PROPERTIES['ID_SERIAL_SHORT']}")
    flash(args.flash_tool, args.boot2, args.partition, args.firmware, args.port)

    # FlashCube has completed. Re-identify before the separate reset/log operation.
    verify_target(args.port)
    captured = capture(args.port, args.timeout, success_prefix)
    args.log.parent.mkdir(parents=True, exist_ok=True)
    args.log.write_bytes(captured)

    safe_output = captured.decode("ascii", errors="replace")
    print(safe_output, end="" if safe_output.endswith("\n") else "\n")
    if success_prefix in captured:
        print(f"PASS: runtime marker captured; log: {args.log}")
        return 0
    if FAILURE_PREFIX in captured:
        print(f"FAIL: target reported smoke failure; log: {args.log}", file=sys.stderr)
        return 1

    print(
        f"FAIL: no runtime marker within {args.timeout:.0f}s; "
        f"log: {args.log}",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
