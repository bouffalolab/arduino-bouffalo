#!/usr/bin/env python3
"""Compile, upload, and validate WiFiS3 scan through the final BL616CL carrier."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial

from run_ra4_wifis3_scan_smoke import (
    COUNT_PATTERN,
    ELAPSED_PATTERN,
    FAIL_PATTERN,
    FIRMWARE_PATTERN,
    PASS_MARKER,
    capture_runtime,
)
from uno_r4_usb_identity import resolve_paired_bl616cl_port, tty_properties


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-scan-smoke"
DEFAULT_BUILD_DIR = ROOT_DIR / "build" / "ra4-wifis3-scan-final-carrier"
DEFAULT_IMAGE = DEFAULT_BUILD_DIR / "ra4-wifis3-scan-smoke.ino.bin"
DEFAULT_UPLOADER = ROOT_DIR / "tools" / "run_ra4_bossac_final_carrier.py"
DEFAULT_FQBN = "arduino:renesas_uno:unor4wifi"


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


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--fqbn", default=DEFAULT_FQBN)
    parser.add_argument("--sketch", type=Path, default=DEFAULT_SKETCH)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    parser.add_argument("--image", type=Path, default=DEFAULT_IMAGE)
    parser.add_argument("--capture-timeout-ms", type=int, default=45000)
    parser.add_argument("--maximum-capture-bytes", type=int, default=8192)
    parser.add_argument("--upload-runtime-settle-ms", type=int, default=500)
    parser.add_argument("--expected-version", default="0.6.0")
    parser.add_argument("--skip-upload", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(args.bridge_port)
    if args.capture_timeout_ms <= 0 or args.maximum_capture_bytes <= 0:
        raise RuntimeError("capture limits must be positive")
    if args.upload_runtime_settle_ms < 0:
        raise RuntimeError("upload runtime settle must not be negative")

    print(f"Bridge: {args.bridge_port} (final-carrier BL616CL 2341:1002)")
    print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")

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
                str(DEFAULT_UPLOADER),
                "--bridge-port",
                args.bridge_port,
                "--image",
                str(image),
                "--runtime-settle-ms",
                str(args.upload_runtime_settle_ms),
            ]
        )

    with serial.Serial(
        args.bridge_port,
        baudrate=115200,
        timeout=0.1,
        write_timeout=1.0,
    ) as bridge:
        bridge.dtr = False
        bridge.rts = False
        bridge.reset_input_buffer()
        runtime = capture_runtime(
            bridge,
            args.capture_timeout_ms / 1000.0,
            args.maximum_capture_bytes,
        )

    if FAIL_PATTERN.search(runtime):
        raise RuntimeError(f"RA4M1 WiFiS3 scan reported failure: {runtime!r}")
    firmware = FIRMWARE_PATTERN.search(runtime)
    count = COUNT_PATTERN.search(runtime)
    elapsed = ELAPSED_PATTERN.search(runtime)
    if firmware is None or count is None or elapsed is None or PASS_MARKER not in runtime:
        raise RuntimeError(f"RA4M1 WiFiS3 scan markers were incomplete: {runtime!r}")
    if firmware.group(1).strip() != args.expected_version:
        raise RuntimeError(f"unexpected bridge firmware version: {firmware.group(1).strip()}")
    network_count = int(count.group(1))
    if network_count <= 0:
        raise RuntimeError(f"WiFiS3 scan returned no networks: {network_count}")

    print(f"RA4_WIFI_S3_FIRMWARE={firmware.group(1).strip()}")
    print(f"RA4_WIFI_S3_SCAN_COUNT={network_count}")
    print(f"RA4_WIFI_S3_SCAN_ELAPSED_MS={elapsed.group(1)}")
    print("RA4_WIFI_S3_SCAN_FINAL_CARRIER_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
