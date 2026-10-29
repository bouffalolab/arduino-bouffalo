#!/usr/bin/env python3
"""Run the BL616CL-owned read-only RA4M1 SAM-BA handshake on the final carrier."""

from __future__ import annotations

import argparse
import array
import fcntl
import os
import time

from inspect_bl616cl_gpio35 import (
    CDC_IN_TELEMETRY_COMMAND,
    FEATURE_BYTES,
    FEATURE_VERSION,
    RA4_SAMBA_PROBE_COMMAND,
    REPORT_BYTES,
    find_hidraw,
    hid_ioc,
    read_feature,
    select_feature_page,
)
from uno_r4_usb_identity import resolve_paired_bl616cl_port, tty_properties


def require_final_carrier_bridge(port: str) -> None:
    properties = tty_properties(port)
    if (
        properties.get("ID_VENDOR_ID", "").lower() != "2341"
        or properties.get("ID_MODEL_ID", "").lower() != "1002"
        or properties.get("ID_USB_INTERFACE_NUM", "").lower() != "00"
        or properties.get("ID_SERIAL_SHORT") != "BL616CL-PROVISIONAL"
    ):
        raise RuntimeError(
            f"{port} is not the final-carrier BL616CL CDC interface"
        )


def read_u32_le(report: bytes, offset: int) -> int:
    return int.from_bytes(report[offset : offset + 4], "little")


def version_summary(report: bytes) -> tuple[str, int, int]:
    prefix = report[30:62].split(b"\0", 1)[0].decode("ascii", errors="replace")
    return prefix, report[62], report[63]


def request_probe(fd: int) -> None:
    feature = array.array("B", [0, RA4_SAMBA_PROBE_COMMAND, 1])
    fcntl.ioctl(fd, hid_ioc(3, 0x06, len(feature)), feature, True)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--timeout-seconds", type=float, default=5.0)
    parser.add_argument("--poll-ms", type=int, default=20)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if args.timeout_seconds <= 0 or args.poll_ms <= 0:
        raise RuntimeError("timeout and poll interval must be positive")

    bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(bridge_port)
    hidraw = find_hidraw()
    if not os.path.exists(hidraw):
        raise RuntimeError(f"missing HID device: {hidraw}")

    print(f"Bridge: {bridge_port} (final-carrier BL616CL 2341:1002)")
    print(f"HID: {hidraw}")
    print(
        "Target: RA4M1 SAM-BA handshake only; no RA4M1 or BL616CL Flash is written."
    )

    fd = os.open(hidraw, os.O_RDWR | os.O_NONBLOCK)
    try:
        select_feature_page(fd, 34)
        before = read_feature(fd)
        if before[:5] != b"U35D" + bytes((FEATURE_VERSION,)):
            raise RuntimeError("unexpected BL616CL diagnostic feature signature")
        sequence = read_u32_le(before, 10)

        request_probe(fd)
        print("RA4_SAMBA_PROBE_REQUESTED")
        deadline = time.monotonic() + args.timeout_seconds
        last_snapshot: tuple[int, int, int, int] | None = None
        while time.monotonic() < deadline:
            select_feature_page(fd, 34)
            report = read_feature(fd)
            if report[:5] != b"U35D" + bytes((FEATURE_VERSION,)):
                raise RuntimeError("BL616CL diagnostic feature signature changed")
            current_sequence = read_u32_le(report, 10)
            snapshot = (current_sequence, report[6], report[7], report[8])
            if snapshot != last_snapshot:
                print(
                    "RA4_SAMBA_PROGRESS="
                    f"sequence={current_sequence} state={report[6]} "
                    f"error={report[7]} active={report[8]} succeeded={report[9]}"
                )
                last_snapshot = snapshot
            if current_sequence > sequence and report[8] == 0:
                version_prefix, version_size, terminator_first = version_summary(report)
                if report[9] != 1 or report[7] != 0:
                    raise RuntimeError(
                        "RA4 SAM-BA handshake failed: "
                        f"error={report[7]} version_prefix={version_prefix!r} "
                        f"version_bytes={version_size}"
                    )
                if (
                    version_size < len("Arduino Bootloader (SAM-BA")
                    or not version_prefix.startswith("Arduino Bootloader (SAM-BA")
                ):
                    raise RuntimeError(
                        "unexpected RA4 SAM-BA version summary: "
                        f"prefix={version_prefix!r} bytes={version_size}"
                    )
                print(
                    "RA4_SAMBA_HANDSHAKE_PASS="
                    f"version_prefix={version_prefix!r} version_bytes={version_size} "
                    f"terminator_first=0x{terminator_first:02x} "
                    f"tx={read_u32_le(report, 22)} rx={read_u32_le(report, 26)}"
                )
                return 0
            time.sleep(args.poll_ms / 1000.0)
        raise RuntimeError("RA4 SAM-BA handshake timed out")
    finally:
        try:
            select_feature_page(fd, 0)
        except OSError:
            pass
        os.close(fd)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError) as error:
        print(f"ERROR: {error}")
        raise SystemExit(1)
