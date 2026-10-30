#!/usr/bin/env python3
"""Reset the paired RA4M1 through split SAM-BA UARTs without writing Flash."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial

from upload_ra4_samba_split import capture, initialize_samba, open_serial, write_all


def capture_until_expected(
    link: serial.Serial,
    duration_seconds: float,
    maximum_bytes: int,
    expected_markers: list[str],
) -> str:
    deadline = time.monotonic() + duration_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        payload = link.read(512)
        if payload:
            received.extend(payload)
            runtime = received.decode("ascii", errors="replace").replace("\x00", "")
            if all(marker in runtime for marker in expected_markers):
                return runtime
            if len(received) > maximum_bytes:
                raise RuntimeError(f"serial capture exceeded {maximum_bytes} byte(s)")
    return received.decode("ascii", errors="replace").replace("\x00", "")


def require_port_identity(port: str, vendor: str, product: str, interface: str | None) -> None:
    device_path = Path(port)
    if not device_path.exists():
        raise RuntimeError(f"serial node does not exist: {port}")

    result = subprocess.run(
        [
            "udevadm",
            "info",
            "--query=property",
            f"--path=/sys/class/tty/{device_path.name}",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"cannot inspect USB identity for {port}: {result.stderr.strip()}")

    properties = dict(
        line.split("=", 1) for line in result.stdout.splitlines() if "=" in line
    )
    actual_vendor = properties.get("ID_VENDOR_ID", "").lower()
    actual_product = properties.get("ID_MODEL_ID", "").lower()
    actual_interface = properties.get("ID_USB_INTERFACE_NUM", "").lower()
    if actual_vendor != vendor.lower() or actual_product != product.lower():
        raise RuntimeError(
            f"{port} has unexpected USB identity: "
            f"{actual_vendor or '<missing>'}:{actual_product or '<missing>'}; "
            f"expected {vendor}:{product}"
        )
    if interface is not None and actual_interface != interface.lower():
        raise RuntimeError(
            f"{port} has unexpected USB interface: "
            f"{actual_interface or '<missing>'}; expected {interface}"
        )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Reset the existing paired RA4M1 application without changing Flash."
    )
    parser.add_argument("--bridge-port", default="/dev/ttyACM0")
    parser.add_argument("--reference-port", default="/dev/ttyUSB0")
    parser.add_argument("--bootstrap-settle-ms", type=int, default=500)
    parser.add_argument("--runtime-settle-ms", type=int, default=300)
    parser.add_argument("--read-timeout-ms", type=int, default=1000)
    parser.add_argument("--capture-ms", type=int, default=45000)
    parser.add_argument("--maximum-capture-bytes", type=int, default=8192)
    parser.add_argument(
        "--expect",
        action="append",
        default=[],
        help="Require this ASCII marker in the post-reset reference-RX capture. Repeatable.",
    )
    parser.add_argument(
        "--stop-when-expectations-met",
        action="store_true",
        help="Stop runtime capture as soon as every --expect marker has been observed.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.bootstrap_settle_ms <= 0
        or args.runtime_settle_ms < 0
        or args.read_timeout_ms <= 0
        or args.capture_ms <= 0
        or args.maximum_capture_bytes <= 0
    ):
        raise RuntimeError("timeouts and capture size must be positive")

    require_port_identity(args.bridge_port, "2341", "1002", "00")
    require_port_identity(args.reference_port, "0403", "6010", "00")
    print(f"Bridge:    {args.bridge_port} (2341:1002 interface 00)")
    print(f"Reference: {args.reference_port} (0403:6010 interface 00)")
    print("Target: existing RA4M1 application only; this command does not erase or program Flash.")
    print("Target: BL616CL Flash is untouched.")
    print("Relay route: RA4M1 UART (RTS=False, physical RTS# HIGH, relay released).")

    reference = open_serial(args.reference_port, 230400, args.read_timeout_ms / 1000)
    bridge: serial.Serial | None = None
    samba_active = False
    runtime_requested = False
    try:
        print(f"RELAY_RUNTIME_ROUTE rts={reference.rts}")
        time.sleep(0.05)
        bridge = open_serial(args.bridge_port, 115200, args.read_timeout_ms / 1000)
        bridge.baudrate = 1200
        print("CDC_SET_LINE_CODING=1200")
        time.sleep(args.bootstrap_settle_ms / 1000)
        bridge.close()

        bridge = open_serial(args.bridge_port, 230400, args.read_timeout_ms / 1000)
        initialize_samba(bridge, reference, args.read_timeout_ms / 1000)
        samba_active = True

        reference.reset_input_buffer()
        write_all(bridge, b"K#")
        runtime_requested = True
        print("SAMBA_RESET_COMMAND=K#")
        time.sleep(0.05)
        reference.baudrate = 115200
        bridge.baudrate = 115200
        time.sleep(args.runtime_settle_ms / 1000)
        print("RA4 runtime requested by K#; user UART restored directly to 115200.")

        if args.stop_when_expectations_met and args.expect:
            runtime = capture_until_expected(
                reference,
                args.capture_ms / 1000,
                args.maximum_capture_bytes,
                args.expect,
            )
        else:
            runtime = capture(
                reference,
                args.capture_ms / 1000,
                args.maximum_capture_bytes,
            ).decode("ascii", errors="replace").replace("\x00", "")
        print("RA4_RUNTIME_CAPTURE_BEGIN")
        print(runtime, end="" if runtime.endswith("\n") else "\n")
        print("RA4_RUNTIME_CAPTURE_END")
        for marker in args.expect:
            if marker not in runtime:
                raise RuntimeError(f"required runtime marker was not observed: {marker}")
        print("RA4_SPLIT_SAMBA_RESET_AND_RUNTIME_PASS")
        return 0
    finally:
        if samba_active and not runtime_requested and bridge is not None and bridge.is_open:
            try:
                write_all(bridge, b"K#")
                print("SAMBA_FAILURE_RECOVERY=K#")
            except serial.SerialException:
                print("SAMBA_FAILURE_RECOVERY=K#_SEND_FAILED", file=sys.stderr)
        if bridge is not None and bridge.is_open:
            bridge.close()
        reference.rts = False
        reference.close()
        print("Relay state: RA4M1 UART runtime route retained (RTS# HIGH, relay released).")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
