#!/usr/bin/env python3
"""Reset and observe the paired RA4M1 over the direct BL616CL UART path."""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import serial

from upload_ra4_samba_direct import initialize_samba, restore_user_uart_baud
from upload_ra4_samba_split import capture, open_serial, write_all
from ra4_relay_route import select_ra4_runtime_route
from uno_r4_usb_identity import resolve_paired_bl616cl_port, resolve_relay_port


def require_port_identity(port: str, vendor: str, product: str, interface: str) -> None:
    device_path = Path(port)
    if not device_path.exists():
        raise RuntimeError(f"serial node does not exist: {port}")
    import subprocess

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
    if (
        properties.get("ID_VENDOR_ID", "").lower() != vendor.lower()
        or properties.get("ID_MODEL_ID", "").lower() != product.lower()
        or properties.get("ID_USB_INTERFACE_NUM", "").lower() != interface.lower()
    ):
        raise RuntimeError(f"{port} has an unexpected USB identity")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument("--capture-ms", type=int, default=45000)
    parser.add_argument("--maximum-capture-bytes", type=int, default=16384)
    parser.add_argument("--runtime-settle-ms", type=int, default=300)
    parser.add_argument(
        "--relay-settle-ms",
        type=int,
        default=500,
        help="Known-route settle time after selecting the RA4M1 relay path.",
    )
    parser.add_argument(
        "--attempts",
        type=int,
        default=1,
        help="Number of bounded no-Flash SAM-BA reset/run attempts.",
    )
    parser.add_argument(
        "--retry-settle-ms",
        type=int,
        default=500,
        help="Delay between failed no-Flash reset/run attempts.",
    )
    parser.add_argument("--expect", action="append", default=[])
    parser.add_argument(
        "--stop-when-expectations-met",
        action="store_true",
        help="finish runtime capture once every --expect marker has been observed",
    )
    return parser.parse_args()


def capture_until_expected(
    link: serial.Serial,
    timeout_seconds: float,
    maximum_bytes: int,
    expected_markers: list[str],
    stop_when_expectations_met: bool,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        payload = link.read(512)
        if payload:
            received.extend(payload)
            if len(received) > maximum_bytes:
                raise RuntimeError("runtime capture exceeded configured limit")
            text = received.decode("ascii", errors="replace").replace("\x00", "")
            if (
                stop_when_expectations_met
                and expected_markers
                and all(marker in text for marker in expected_markers)
            ):
                return text
    return received.decode("ascii", errors="replace").replace("\x00", "")


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    if (
        args.capture_ms <= 0
        or args.maximum_capture_bytes <= 0
        or args.attempts <= 0
        or args.retry_settle_ms < 0
        or args.relay_settle_ms < 0
    ):
        raise RuntimeError("capture limits must be positive")
    require_port_identity(args.bridge_port, "2341", "1002", "00")
    require_port_identity(args.relay_port, "0403", "6010", "00")

    relay = serial.Serial(args.relay_port, 115200, timeout=0.1, write_timeout=1)
    relay.dtr = False
    relay.rts = False
    last_error: BaseException | None = None
    try:
        print(f"Bridge: {args.bridge_port} (2341:1002 interface 00)")
        print(f"Relay: {args.relay_port} (0403:6010 interface 00)")
        print("Target: existing RA4M1 application only; no Flash is written.")
        print("Relay route: RA4M1 runtime (RTS=False, physical RTS# HIGH, released).")

        for attempt in range(1, args.attempts + 1):
            bridge: serial.Serial | None = None
            samba_may_be_active = False
            runtime_requested = False
            try:
                print(f"RA4_DIRECT_RESET_ATTEMPT={attempt}/{args.attempts}")
                select_ra4_runtime_route(relay, args.relay_settle_ms)
                print(
                    "RELAY_RUNTIME_ROUTE="
                    f"known-state settle_ms={args.relay_settle_ms}"
                )
                bridge = open_serial(args.bridge_port, 115200, 1.0)
                bridge.baudrate = 1200
                print("CDC_SET_LINE_CODING=1200")
                time.sleep(0.5)
                bridge.close()
                bridge = open_serial(args.bridge_port, 230400, 1.0)
                samba_may_be_active = True
                initialize_samba(bridge, 1.0, require_version=False)
                write_all(bridge, b"K#")
                runtime_requested = True
                print("SAMBA_RESET_COMMAND=K#")
                time.sleep(0.05)
                bridge.baudrate = 115200
                time.sleep(args.runtime_settle_ms / 1000)
                runtime = capture_until_expected(
                    bridge,
                    args.capture_ms / 1000,
                    args.maximum_capture_bytes,
                    args.expect,
                    args.stop_when_expectations_met,
                )
                print("RA4_RUNTIME_CAPTURE_BEGIN")
                print(runtime, end="" if runtime.endswith("\n") else "\n")
                print("RA4_RUNTIME_CAPTURE_END")
                for marker in args.expect:
                    if marker not in runtime:
                        raise RuntimeError(
                            f"required runtime marker was not observed: {marker}"
                        )
                print("RA4_DIRECT_SAMBA_RESET_AND_RUNTIME_PASS")
                return 0
            except (OSError, RuntimeError, serial.SerialException) as error:
                last_error = error
                print(
                    f"RA4_DIRECT_RESET_ATTEMPT_FAILED={attempt}/{args.attempts}: {error}",
                    file=sys.stderr,
                )
            finally:
                if (
                    samba_may_be_active
                    and not runtime_requested
                    and bridge is not None
                    and bridge.is_open
                ):
                    try:
                        write_all(bridge, b"K#")
                        print("SAMBA_FAILURE_RECOVERY=K#")
                    except serial.SerialException:
                        print("SAMBA_FAILURE_RECOVERY=K#_SEND_FAILED", file=sys.stderr)
                if bridge is not None and bridge.is_open:
                    bridge.close()
                if not runtime_requested:
                    try:
                        restore_user_uart_baud(
                            args.bridge_port,
                            1.0,
                            args.runtime_settle_ms / 1000,
                        )
                    except serial.SerialException:
                        print("CDC_FAILURE_RECOVERY=FAILED", file=sys.stderr)

            if attempt < args.attempts:
                time.sleep(args.retry_settle_ms / 1000)

        assert last_error is not None
        raise last_error
    finally:
        relay.rts = False
        relay.close()
        print("Relay state: RA4M1 runtime route retained (RTS# HIGH, released).")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException) as error:
        print(f"ERROR: {error}")
        raise SystemExit(1)
