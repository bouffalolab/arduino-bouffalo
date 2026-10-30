#!/usr/bin/env python3
"""Validate a redacted paired-board AT+WIFISCAN path through RA4M1 Serial2.

The default data path is the paired BL616CL CDC/GPIO34/GPIO35 user UART. This
runner sends one credential-free scan request through the temporary RA4M1
proxy, validates framing without printing SSIDs or BSSIDs, then checks status.
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

import serial

from ra4_runtime_uart_route import open_runtime_route
from uno_r4_usb_identity import resolve_paired_bl616cl_port, resolve_relay_port


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_AT_GATE = ROOT_DIR / "tools" / "run_ra4_at_uart_gate.py"
SCAN_REQUEST = b"AT+WIFISCAN\r\n"
STATUS_REQUEST = b"AT+GETSTATUS?\r\n"
SCAN_PREFIX = b"+WIFISCAN: "
TERMINAL = b"OK\r\n"
STATUS_RESPONSE = b"+GETSTATUS: 2\r\nOK\r\n"
ROW_DELIMITER = b" | "
BSSID_PATTERN = re.compile(rb"^[0-9a-f]{2}(?::[0-9a-f]{2}){5}$")


def require_port_identity(port: str, vendor: str, product: str, interface: str | None) -> None:
    device_path = Path(port)
    if not device_path.exists():
        raise RuntimeError(f"serial node does not exist: {port}")

    tty_name = device_path.name
    result = subprocess.run(
        [
            "udevadm",
            "info",
            "--query=property",
            f"--path=/sys/class/tty/{tty_name}",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"cannot inspect USB identity for {port}: {result.stderr.strip()}")

    properties = {}
    for line in result.stdout.splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            properties[key] = value

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


def read_until_terminal(
    link: serial.Serial,
    timeout_seconds: float,
    maximum_bytes: int,
) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        chunk = link.read(max(1, min(512, maximum_bytes - len(received))))
        if chunk:
            received.extend(chunk)
            if len(received) > maximum_bytes:
                raise RuntimeError(f"scan response exceeded {maximum_bytes} byte limit")
            if received.endswith(TERMINAL):
                return bytes(received)
    raise RuntimeError(
        f"scan response timed out after {timeout_seconds:.1f} seconds "
        f"with {len(received)} byte(s)"
    )


def read_exact(link: serial.Serial, expected: bytes, timeout_seconds: float) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while len(received) < len(expected) and time.monotonic() < deadline:
        payload = link.read(len(expected) - len(received))
        if payload:
            received.extend(payload)
    return bytes(received)


def validate_scan_response(response: bytes) -> int:
    if response == TERMINAL:
        return 0
    if not response.startswith(SCAN_PREFIX) or not response.endswith(TERMINAL):
        raise RuntimeError("scan response did not match the expected AT framing")

    separator_at = response.find(b"|", len(SCAN_PREFIX))
    if separator_at < 0:
        raise RuntimeError("scan response did not contain a payload length")
    length_text = response[len(SCAN_PREFIX) : separator_at]
    if not length_text.isdigit():
        raise RuntimeError("scan response payload length was not decimal")
    payload = response[separator_at + 1 : -len(TERMINAL)]
    if int(length_text) != len(payload):
        raise RuntimeError("scan response payload length did not match its body")

    rows = payload.split(b"\r\n")
    if not rows or rows[-1] != b"":
        raise RuntimeError("scan response rows were not CRLF terminated")

    for row in rows[:-1]:
        fields = row.split(ROW_DELIMITER)
        if len(fields) != 5 or not fields[0] or not BSSID_PATTERN.fullmatch(fields[1]):
            raise RuntimeError("scan response contained an invalid row shape")
        try:
            int(fields[2])
            int(fields[3])
            fields[4].decode("ascii", "strict")
        except (UnicodeDecodeError, ValueError) as error:
            raise RuntimeError("scan response contained invalid numeric or text fields") from error

    return len(rows) - 1


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
    ]
    print("COMMAND=" + " ".join(command))
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"RA4M1 proxy upload gate failed with exit status {result.returncode}")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run a redacted paired-board BL616CL AT+WIFISCAN hardware gate."
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
    parser.add_argument("--scan-timeout-ms", type=int, default=25000)
    parser.add_argument("--status-timeout-ms", type=int, default=2000)
    parser.add_argument("--maximum-response-bytes", type=int, default=24576)
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
    if (
        args.scan_timeout_ms <= 0
        or args.status_timeout_ms <= 0
        or args.maximum_response_bytes < len(TERMINAL)
        or args.runtime_settle_ms < 0
    ):
        raise RuntimeError("invalid timeout or response size argument")

    require_port_identity(args.bridge_port, "2341", "1002", "00")
    require_port_identity(args.relay_port, "0403", "6010", "00")
    if args.legacy_reference_port is not None:
        require_port_identity(args.legacy_reference_port, "0403", "6010", "00")
    print(f"Bridge:    {args.bridge_port} (2341:1002 interface 00)")
    print(f"Relay:     {args.relay_port} (0403:6010 interface 00)")
    print("Relay route: RA4M1 UART (RTS=False, physical RTS# HIGH, relay released).")
    print("Privacy: scan rows are validated in memory and never printed or written.")

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
    try:
        bridge.reset_input_buffer()
        reference.reset_input_buffer()

        started_at = time.monotonic()
        bridge.write(SCAN_REQUEST)
        bridge.flush()
        scan_response = read_until_terminal(
            reference,
            args.scan_timeout_ms / 1000.0,
            args.maximum_response_bytes,
        )
        elapsed_ms = (time.monotonic() - started_at) * 1000.0
        row_count = validate_scan_response(scan_response)
        print(f"WIFISCAN_ROWS={row_count}")
        print(f"WIFISCAN_RESPONSE_BYTES={len(scan_response)}")
        print(f"WIFISCAN_ELAPSED_MS={elapsed_ms:.3f}")
        print("WIFISCAN_TERMINAL=OK")

        reference.reset_input_buffer()
        bridge.write(STATUS_REQUEST)
        bridge.flush()
        status_response = read_exact(
            reference,
            STATUS_RESPONSE,
            args.status_timeout_ms / 1000.0,
        )
        if status_response != STATUS_RESPONSE:
            raise RuntimeError(
                "unexpected post-scan GETSTATUS response: "
                f"received {len(status_response)} byte(s), expected scan-complete status"
            )
        print("GETSTATUS_AFTER_SCAN=2")
        print("RA4_AT_WIFISCAN_GPIO6_GPIO7_PASS")
        return 0
    finally:
        route.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
