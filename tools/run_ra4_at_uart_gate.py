#!/usr/bin/env python3
"""Validate the BL616CL GPIO6/GPIO7 RA4M1 AT UART hardware gate.

The default route uses BL616CL GPIO34/GPIO35 in both directions. The temporary
RA4M1 proxy forwards Serial <-> Serial2 so this runner can verify exact AT
requests and responses with the bridge over UART1 GPIO6/GPIO7.
"""

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
DEFAULT_FQBN = "arduino:renesas_uno:unor4wifi"
DEFAULT_BUILD_DIR = ROOT_DIR / "build" / "ra4-at-transcript-proxy"
DEFAULT_SKETCH = ROOT_DIR / "examples" / "ra4-at-transcript-proxy"
DEFAULT_IMAGE = DEFAULT_BUILD_DIR / "ra4-at-transcript-proxy.ino.bin"
DEFAULT_SAMBA_RUNNER = ROOT_DIR / "tools" / "upload_ra4_samba_direct.py"
DEFAULT_RUNTIME_SETTLE_MS = 2000

REQUESTS = (
    (b"AT\r\n", b"OK\r\n"),
    (b"AT+FWVERSION?\r\n", b"+FWVERSION: 0.6.0\r\nOK\r\n"),
    (b"AT+GETSTATUS?\r\n", b"+GETSTATUS: 0\r\nOK\r\n"),
)
MAXIMUM_QUERY_ATTEMPTS = 3
QUERY_RETRY_DELAY_SECONDS = 0.25


def require_port_identity(port: str, vendor: str, product: str, interface: str | None) -> None:
    device_path = Path(port)
    if not device_path.exists():
        raise RuntimeError(f"serial node does not exist: {port}")

    tty_name = device_path.name
    property_path = Path("/sys/class/tty") / tty_name / "device" / "uevent"
    if not property_path.is_file():
        raise RuntimeError(f"serial sysfs node does not exist: {port}")

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


def read_exact(link: serial.Serial, expected: bytes, timeout_seconds: float) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while len(received) < len(expected) and time.monotonic() < deadline:
        payload = link.read(len(expected) - len(received))
        if payload:
            received.extend(payload)
    return bytes(received)


def transact_exact_query(
    bridge: serial.Serial,
    receiver: serial.Serial,
    request: bytes,
    expected: bytes,
    timeout_seconds: float,
) -> bytes:
    """Send an idempotent preflight query, tolerating post-reset CDC handoff."""
    actual = b""
    for attempt in range(MAXIMUM_QUERY_ATTEMPTS):
        receiver.reset_input_buffer()
        bridge.write(request)
        bridge.flush()
        actual = read_exact(receiver, expected, timeout_seconds)
        if actual == expected:
            return actual
        if attempt + 1 < MAXIMUM_QUERY_ATTEMPTS:
            time.sleep(QUERY_RETRY_DELAY_SECONDS)
    return actual


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"command failed with exit status {result.returncode}")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build, upload, and validate the paired-board BL616CL RA4 AT UART gate."
    )
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument(
        "--reference-port",
        dest="relay_port",
        default=argparse.SUPPRESS,
        help=argparse.SUPPRESS,
    )
    parser.add_argument(
        "--legacy-reference-port",
        help="Receive through a separate legacy FTDI path instead of BL616CL GPIO35.",
    )
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--fqbn", default=DEFAULT_FQBN)
    parser.add_argument("--sketch", type=Path, default=DEFAULT_SKETCH)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    parser.add_argument("--image", type=Path, default=DEFAULT_IMAGE)
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--boot-settle-ms", type=int, default=500)
    parser.add_argument("--runtime-settle-ms", type=int, default=DEFAULT_RUNTIME_SETTLE_MS)
    parser.add_argument(
        "--skip-status-check",
        action="store_true",
        help=(
            "Skip the fixed GETSTATUS=0 assertion after proxy upload. Use only "
            "when a following stateful gate validates status explicitly."
        ),
    )
    parser.add_argument(
        "--skip-upload",
        action="store_true",
        help="Do not build or upload the RA4M1 proxy; validate the currently running proxy.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    if (
        args.response_timeout_ms <= 0
        or args.boot_settle_ms < 0
        or args.runtime_settle_ms < 0
    ):
        raise RuntimeError("timeouts must be positive")

    require_port_identity(args.bridge_port, "2341", "1002", "00")
    require_port_identity(args.relay_port, "0403", "6010", "00")
    if args.legacy_reference_port is not None:
        require_port_identity(args.legacy_reference_port, "0403", "6010", "00")
    print(f"Bridge:    {args.bridge_port} (2341:1002 interface 00)")
    print(f"Relay:     {args.relay_port} (0403:6010 interface 00)")
    print("Relay route: RA4M1 UART (RTS=False, physical RTS# HIGH, relay released).")

    if not args.skip_upload:
        sketch = args.sketch.resolve()
        build_dir = args.build_dir.resolve()
        image = args.image.resolve()
        if not sketch.is_dir():
            raise RuntimeError(f"RA4M1 proxy Sketch directory does not exist: {sketch}")

        run_command(
            [
                args.arduino_cli,
                "compile",
                "--fqbn",
                args.fqbn,
                "--build-path",
                str(build_dir),
                str(sketch),
            ]
        )
        if not image.is_file():
            raise RuntimeError(f"compiled RA4M1 proxy image does not exist: {image}")
        run_command(
            [
                sys.executable,
                str(DEFAULT_SAMBA_RUNNER),
                "--bridge-port",
                args.bridge_port,
                "--relay-port",
                args.relay_port,
                "--image",
                str(image),
                "--bootstrap-settle-ms",
                str(args.boot_settle_ms),
                "--runtime-settle-ms",
                str(args.runtime_settle_ms),
                "--skip-runtime-capture",
            ]
        )

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
        requests = REQUESTS if not args.skip_status_check else REQUESTS[:2]
        for request, expected in requests:
            actual = transact_exact_query(
                bridge,
                reference,
                request,
                expected,
                args.response_timeout_ms / 1000.0,
            )
            print(f"AT_REQUEST={request!r}")
            print(f"AT_RESPONSE={actual!r}")
            if actual != expected:
                raise RuntimeError(
                    f"unexpected response for {request!r}: expected {expected!r}, got {actual!r}"
                )
        print("RA4_AT_UART_GPIO6_GPIO7_PASS")
        return 0
    finally:
        route.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
