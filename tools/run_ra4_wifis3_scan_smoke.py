#!/usr/bin/env python3
"""Upload and validate a redacted RA4M1 WiFiS3 scan smoke Sketch."""

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
DEFAULT_SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-scan-smoke"
DEFAULT_BUILD_DIR = ROOT_DIR / "build" / "ra4-wifis3-scan-smoke"
DEFAULT_IMAGE = DEFAULT_BUILD_DIR / "ra4-wifis3-scan-smoke.ino.bin"
DEFAULT_SAMBA_RUNNER = ROOT_DIR / "tools" / "upload_ra4_samba_direct.py"
DEFAULT_FQBN = "arduino:renesas_uno:unor4wifi"

FIRMWARE_PATTERN = re.compile(r"^RA4_WIFI_S3_SCAN:FIRMWARE=(.+?)\r?$", re.MULTILINE)
COUNT_PATTERN = re.compile(r"^RA4_WIFI_S3_SCAN:COUNT=(-?\d+)\r?$", re.MULTILINE)
ELAPSED_PATTERN = re.compile(r"^RA4_WIFI_S3_SCAN:ELAPSED_MS=(\d+)\r?$", re.MULTILINE)
PASS_MARKER = "RA4_WIFI_S3_SCAN:RESULT=PASS"
FAIL_PATTERN = re.compile(r"^RA4_WIFI_S3_SCAN:RESULT=FAIL:.*\r?$", re.MULTILINE)


def has_complete_success_markers(runtime: str) -> bool:
    return (
        FIRMWARE_PATTERN.search(runtime) is not None
        and COUNT_PATTERN.search(runtime) is not None
        and ELAPSED_PATTERN.search(runtime) is not None
        and PASS_MARKER in runtime
    )


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


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"command failed with exit status {result.returncode}")


def capture_runtime(link: serial.Serial, timeout_seconds: float, maximum_bytes: int) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        payload = link.read(512)
        if payload:
            received.extend(payload)
            if len(received) > maximum_bytes:
                raise RuntimeError(f"runtime capture exceeded {maximum_bytes} bytes")
            text = received.decode("ascii", errors="replace").replace("\x00", "")
            if has_complete_success_markers(text) or FAIL_PATTERN.search(text):
                return text
    return received.decode("ascii", errors="replace").replace("\x00", "")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compile, upload, and validate the RA4M1 WiFiS3 scan smoke."
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
    parser.add_argument("--capture-timeout-ms", type=int, default=45000)
    parser.add_argument("--maximum-capture-bytes", type=int, default=8192)
    parser.add_argument("--upload-runtime-settle-ms", type=int, default=100)
    parser.add_argument(
        "--skip-upload",
        action="store_true",
        help="Validate the currently running WiFiS3 scan Sketch without replacing RA4M1 Flash.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    if args.capture_timeout_ms <= 0 or args.maximum_capture_bytes <= 0:
        raise RuntimeError("capture limits must be positive")
    if args.upload_runtime_settle_ms < 0:
        raise RuntimeError("upload runtime settle must not be negative")

    require_port_identity(args.bridge_port, "2341", "1002", "00")
    require_port_identity(args.relay_port, "0403", "6010", "00")
    if args.legacy_reference_port is not None:
        require_port_identity(args.legacy_reference_port, "0403", "6010", "00")
    print(f"Bridge:    {args.bridge_port} (2341:1002)")
    print(f"Relay:     {args.relay_port} (0403:6010 interface 00)")
    print(
        "Runtime route: direct BL616CL GPIO34/GPIO35"
        if args.legacy_reference_port is None
        else f"Runtime route: legacy receiver {args.legacy_reference_port}"
    )
    print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Relay route: RA4M1 UART (RTS=False, physical RTS# HIGH, relay released).")
    print("Privacy: only sanitized Sketch markers are captured.")

    if not args.skip_upload:
        sketch = args.sketch.resolve()
        build_dir = args.build_dir.resolve()
        image = args.image.resolve()
        if not sketch.is_dir():
            raise RuntimeError(f"Sketch directory does not exist: {sketch}")

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
            raise RuntimeError(f"compiled Sketch image does not exist: {image}")

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
                "--runtime-settle-ms",
                str(args.upload_runtime_settle_ms),
                "--skip-runtime-capture",
            ]
        )

    route = open_runtime_route(
        args.bridge_port,
        args.relay_port,
        args.legacy_reference_port,
        bridge_timeout=1.0,
        receiver_timeout=0.1,
    )
    try:
        runtime = capture_runtime(
            route.receiver,
            args.capture_timeout_ms / 1000.0,
            args.maximum_capture_bytes,
        )
    finally:
        route.close()

    if FAIL_PATTERN.search(runtime):
        failure = FAIL_PATTERN.search(runtime)
        raise RuntimeError(f"RA4M1 WiFiS3 scan reported failure: {failure.group(0)}")
    firmware = FIRMWARE_PATTERN.search(runtime)
    count = COUNT_PATTERN.search(runtime)
    elapsed = ELAPSED_PATTERN.search(runtime)
    if firmware is None or count is None or elapsed is None or PASS_MARKER not in runtime:
        raise RuntimeError("RA4M1 WiFiS3 scan markers were incomplete")
    if firmware.group(1).strip() != "0.6.0":
        raise RuntimeError(f"unexpected bridge firmware version: {firmware.group(1).strip()}")
    network_count = int(count.group(1))
    if network_count <= 0:
        raise RuntimeError(f"WiFiS3 scan returned no networks: {network_count}")

    print(f"RA4_WIFI_S3_FIRMWARE={firmware.group(1).strip()}")
    print(f"RA4_WIFI_S3_SCAN_COUNT={network_count}")
    print(f"RA4_WIFI_S3_SCAN_ELAPSED_MS={elapsed.group(1)}")
    print("RA4_WIFI_S3_SCAN_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
