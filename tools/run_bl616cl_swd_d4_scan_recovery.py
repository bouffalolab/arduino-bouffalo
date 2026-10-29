#!/usr/bin/env python3
"""Require WiFiS3 scan recovery across the final-carrier BL616CL D4 flow."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

import serial

from read_bl616cl_windows_telemetry import (
    resolve_wsl_usbip_host,
)
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
TELEMETRY_RUNNER = ROOT_DIR / "tools" / "read_bl616cl_windows_telemetry.py"


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--capture-timeout-ms", type=int, default=45000)
    parser.add_argument("--maximum-capture-bytes", type=int, default=8192)
    return parser.parse_args()


def require_final_carrier(port: str) -> None:
    properties = tty_properties(port)
    if (
        properties.get("ID_VENDOR_ID", "").lower() != "2341"
        or properties.get("ID_MODEL_ID", "").lower() != "1002"
        or properties.get("ID_USB_INTERFACE_NUM", "").lower() != "00"
        or properties.get("ID_SERIAL_SHORT") != "BL616CL-PROVISIONAL"
    ):
        raise RuntimeError(f"{port} is not the final-carrier BL616CL CDC interface")


def capture_scan_result(port: str, timeout_seconds: float, maximum_bytes: int) -> str:
    with serial.Serial(
        port,
        baudrate=115200,
        timeout=0.1,
        write_timeout=1.0,
    ) as bridge:
        bridge.dtr = False
        bridge.rts = False
        bridge.reset_input_buffer()
        return capture_runtime(bridge, timeout_seconds, maximum_bytes)


def require_scan_pass(runtime: str, phase: str) -> tuple[str, int, int]:
    failure = FAIL_PATTERN.search(runtime)
    if failure is not None:
        raise RuntimeError(f"{phase} WiFiS3 scan failed: {failure.group(0)}")
    firmware = FIRMWARE_PATTERN.search(runtime)
    count = COUNT_PATTERN.search(runtime)
    elapsed = ELAPSED_PATTERN.search(runtime)
    if firmware is None or count is None or elapsed is None or PASS_MARKER not in runtime:
        raise RuntimeError(f"{phase} WiFiS3 scan markers were incomplete")
    if firmware.group(1).strip() != "0.6.0":
        raise RuntimeError(
            f"{phase} unexpected bridge firmware version: {firmware.group(1).strip()}"
        )
    network_count = int(count.group(1))
    if network_count <= 0:
        raise RuntimeError(f"{phase} WiFiS3 scan returned no networks: {network_count}")
    return firmware.group(1).strip(), network_count, int(elapsed.group(1))


def main() -> int:
    args = parse_arguments()
    if args.capture_timeout_ms <= 0 or args.maximum_capture_bytes <= 0:
        raise RuntimeError("capture limits must be positive")

    port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier(port)
    timeout_seconds = args.capture_timeout_ms / 1000.0
    print(f"BL616CL_FINAL_CARRIER_PORT={port}")
    print("Target: RA4M1 D4 halt/run/raw nRESET and WiFiS3 scan only; no Flash is written.")

    before_runtime = capture_scan_result(port, timeout_seconds, args.maximum_capture_bytes)
    firmware, before_count, before_elapsed = require_scan_pass(before_runtime, "pre-D4")
    print(f"BL616CL_D4_SCAN_BEFORE_FIRMWARE={firmware}")
    print(f"BL616CL_D4_SCAN_BEFORE_COUNT={before_count}")
    print(f"BL616CL_D4_SCAN_BEFORE_ELAPSED_MS={before_elapsed}")

    command = [
        sys.executable,
        str(TELEMETRY_RUNNER),
        "swd-d4-halt-run-disconnect-delay",
        "--bridge-port",
        port,
        "--wsl-usbip-host",
        resolve_wsl_usbip_host("auto"),
    ]
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("CMSIS-DAP D4 transport runner failed")
    require_final_carrier(port)

    after_runtime = capture_scan_result(port, timeout_seconds, args.maximum_capture_bytes)
    firmware, after_count, after_elapsed = require_scan_pass(after_runtime, "post-D4")
    print(f"BL616CL_D4_SCAN_AFTER_FIRMWARE={firmware}")
    print(f"BL616CL_D4_SCAN_AFTER_COUNT={after_count}")
    print(f"BL616CL_D4_SCAN_AFTER_ELAPSED_MS={after_elapsed}")
    print("BL616CL_RA4_SWD_D4_WIFIS3_SCAN_RECOVERY_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
