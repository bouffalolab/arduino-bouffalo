#!/usr/bin/env python3
"""Validate the paired-board WIFIMODE 1 -> 0 -> 1 compatibility contract."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial

from ra4_runtime_uart_route import open_runtime_route
from uno_r4_usb_identity import resolve_paired_bl616cl_port, resolve_relay_port


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_AT_GATE = ROOT_DIR / "tools" / "run_ra4_at_uart_gate.py"

MODE_READ_REQUEST = b"AT+WIFIMODE?\r\n"
MODE_WRITE_NULL_REQUEST = b"AT+WIFIMODE=0\r\n"
MODE_WRITE_STA_REQUEST = b"AT+WIFIMODE=1\r\n"
MODE_READ_STA_RESPONSE = b"+WIFIMODE: 1\r\n\r\nOK\r\n"
MODE_READ_NULL_RESPONSE = b"+WIFIMODE: 0\r\n\r\nOK\r\n"
MODE_WRITE_RESPONSE = b"+WIFIMODE: \r\nOK\r\n"
STATUS_REQUEST = b"AT+GETSTATUS?\r\n"
STATUS_NULL_RESPONSE = b"+GETSTATUS: 255\r\nOK\r\n"
STATUS_IDLE_RESPONSE = b"+GETSTATUS: 0\r\nOK\r\n"


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


def open_serial(port: str, baudrate: int, timeout: float) -> serial.Serial:
    link = serial.Serial(
        port,
        baudrate=baudrate,
        timeout=timeout,
        write_timeout=timeout,
    )
    link.dtr = False
    link.rts = False
    return link


def read_exact(link: serial.Serial, expected: bytes, timeout_seconds: float) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while len(received) < len(expected) and time.monotonic() < deadline:
        payload = link.read(len(expected) - len(received))
        if payload:
            received.extend(payload)
    return bytes(received)


def expect_response(
    bridge: serial.Serial,
    reference: serial.Serial,
    request: bytes,
    expected: bytes,
    timeout_seconds: float,
    label: str,
) -> None:
    reference.reset_input_buffer()
    bridge.write(request)
    bridge.flush()
    actual = read_exact(reference, expected, timeout_seconds)
    print(f"{label}_REQUEST={request!r}")
    print(f"{label}_RESPONSE={actual!r}")
    if actual != expected:
        raise RuntimeError(
            f"{label} returned unexpected response: expected {expected!r}, got {actual!r}"
        )


def run_proxy_upload(args: argparse.Namespace) -> None:
    command = [
        sys.executable,
        str(DEFAULT_AT_GATE),
        "--bridge-port",
        args.bridge_port,
        "--relay-port",
        args.relay_port,
        "--arduino-cli",
        args.arduino_cli,
        "--runtime-settle-ms",
        str(args.runtime_settle_ms),
        "--skip-status-check",
    ]
    print("COMMAND=" + " ".join(command))
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"RA4M1 proxy upload gate failed with exit status {result.returncode}")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Validate paired-board BL616CL WIFIMODE 1 -> 0 -> 1 with recovery."
    )
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument(
        "--reference-port",
        dest="relay_port",
        default=argparse.SUPPRESS,
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--legacy-reference-port")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--runtime-settle-ms", type=int, default=2000)
    parser.add_argument(
        "--upload-proxy",
        action="store_true",
        help="Build and upload the temporary RA4M1 Serial <-> Serial2 proxy first.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    if args.response_timeout_ms <= 0 or args.runtime_settle_ms < 0:
        raise RuntimeError("timeouts must be positive")

    require_port_identity(args.bridge_port, "2341", "1002", "00")
    require_port_identity(args.relay_port, "0403", "6010", "00")
    if args.legacy_reference_port is not None:
        require_port_identity(args.legacy_reference_port, "0403", "6010", "00")
    print(f"Bridge:    {args.bridge_port} (2341:1002 interface 00)")
    print(f"Relay:     {args.relay_port} (0403:6010 interface 00)")
    print("Target: RA4M1 proxy application Flash only when --upload-proxy is used.")
    print("Target: BL616CL Flash is untouched.")
    print("Relay route: RA4M1 UART (RTS=False, physical RTS# HIGH, relay released).")

    if args.upload_proxy:
        run_proxy_upload(args)

    route = open_runtime_route(
        args.bridge_port,
        args.relay_port,
        args.legacy_reference_port,
        bridge_timeout=1.0,
        receiver_timeout=0.05,
    )
    bridge = route.bridge
    reference = route.receiver
    timeout_seconds = args.response_timeout_ms / 1000.0
    sta_restore_confirmed = False
    try:
        bridge.reset_input_buffer()
        reference.reset_input_buffer()

        expect_response(
            bridge,
            reference,
            MODE_READ_REQUEST,
            MODE_READ_STA_RESPONSE,
            timeout_seconds,
            "WIFIMODE_PREFLIGHT",
        )
        expect_response(
            bridge,
            reference,
            MODE_WRITE_NULL_REQUEST,
            MODE_WRITE_RESPONSE,
            timeout_seconds,
            "WIFIMODE_NULL",
        )
        expect_response(
            bridge,
            reference,
            MODE_READ_REQUEST,
            MODE_READ_NULL_RESPONSE,
            timeout_seconds,
            "WIFIMODE_NULL_READBACK",
        )
        expect_response(
            bridge,
            reference,
            STATUS_REQUEST,
            STATUS_NULL_RESPONSE,
            timeout_seconds,
            "GETSTATUS_NULL",
        )
        expect_response(
            bridge,
            reference,
            MODE_WRITE_STA_REQUEST,
            MODE_WRITE_RESPONSE,
            timeout_seconds,
            "WIFIMODE_STA_RESTORE",
        )
        expect_response(
            bridge,
            reference,
            MODE_READ_REQUEST,
            MODE_READ_STA_RESPONSE,
            timeout_seconds,
            "WIFIMODE_STA_READBACK",
        )
        expect_response(
            bridge,
            reference,
            STATUS_REQUEST,
            STATUS_IDLE_RESPONSE,
            timeout_seconds,
            "GETSTATUS_STA",
        )
        sta_restore_confirmed = True
        print("RA4_AT_WIFIMODE_1_0_1_PASS")
        return 0
    finally:
        if not sta_restore_confirmed:
            try:
                expect_response(
                    bridge,
                    reference,
                    MODE_WRITE_STA_REQUEST,
                    MODE_WRITE_RESPONSE,
                    timeout_seconds,
                    "WIFIMODE_FAILURE_RECOVERY",
                )
                print("WIFIMODE_FAILURE_RECOVERY=STA")
            except (OSError, RuntimeError, serial.SerialException) as error:
                print(f"WIFIMODE_FAILURE_RECOVERY_ERROR={error}", file=sys.stderr)
        route.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
