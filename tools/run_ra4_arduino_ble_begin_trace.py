#!/usr/bin/env python3
"""Trace ArduinoBLE begin HCI traffic on the paired board, then restore WiFiS3."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_SKETCH = ROOT_DIR / "examples" / "ra4-arduino-ble-begin-trace"
DEFAULT_BUILD_DIR = ROOT_DIR / "build" / "ra4-arduino-ble-begin-trace"
DEFAULT_IMAGE = DEFAULT_BUILD_DIR / "ra4-arduino-ble-begin-trace.ino.bin"
DEFAULT_BOSSAC_RUNNER = ROOT_DIR / "tools" / "run_ra4_bossac_direct.py"
DEFAULT_RESET_RUNNER = ROOT_DIR / "tools" / "reset_ra4_samba_direct.py"
DEFAULT_RESTORE_RUNNER = ROOT_DIR / "tools" / "run_ra4_wifis3_scan_smoke.py"


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"command failed with exit status {result.returncode}")


def run_capture(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(DEFAULT_RESET_RUNNER),
            "--bridge-port",
            args.bridge_port,
            "--relay-port",
            args.relay_port,
            "--capture-ms",
            str(args.capture_ms),
            "--expect",
            "RA4_BLE_BEGIN_TRACE:BOOT",
            "--expect",
            "RA4_BLE_BEGIN_TRACE:BLE_BEGIN=",
            "--expect",
            "RA4_BLE_BEGIN_TRACE:DONE",
            "--expect",
            "RA4_BLE_BEGIN_TRACE:RESULT=",
        ]
    )


def restore_wifis3_scan(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(DEFAULT_RESTORE_RUNNER),
            "--bridge-port",
            args.bridge_port,
            "--relay-port",
            args.relay_port,
            "--arduino-cli",
            args.arduino_cli,
        ]
    )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Trace the paired-board ArduinoBLE BLE.begin() HCI exchange."
    )
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument(
        "--reference-port",
        dest="relay_port",
        default=argparse.SUPPRESS,
        help=argparse.SUPPRESS,
    )
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--capture-ms", type=int, default=8000)
    parser.add_argument("--upload-runtime-settle-ms", type=int, default=150)
    parser.add_argument("--post-upload-settle-ms", type=int, default=2000)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if args.capture_ms <= 0 or args.upload_runtime_settle_ms < 0 or args.post_upload_settle_ms < 0:
        raise RuntimeError("capture timeout must be positive and settle times non-negative")

    primary_error: BaseException | None = None
    try:
        run_command(
            [
                args.arduino_cli,
                "compile",
                "--fqbn",
                "arduino:renesas_uno:unor4wifi",
                "--libraries",
                str(ROOT_DIR / "libraries"),
                "--build-path",
                str(DEFAULT_BUILD_DIR),
                str(DEFAULT_SKETCH),
            ]
        )
        if not DEFAULT_IMAGE.is_file():
            raise RuntimeError(f"compiled trace image does not exist: {DEFAULT_IMAGE}")
        run_command(
            [
                sys.executable,
                str(DEFAULT_BOSSAC_RUNNER),
                "--bridge-port",
                args.bridge_port,
                "--relay-port",
                args.relay_port,
                "--image",
                str(DEFAULT_IMAGE),
                "--runtime-settle-ms",
                str(args.upload_runtime_settle_ms),
                "--skip-runtime-smoke",
            ]
        )
        if args.post_upload_settle_ms:
            time.sleep(args.post_upload_settle_ms / 1000)
        run_capture(args)
    except BaseException as error:
        primary_error = error
    finally:
        try:
            print("RA4_WIFI_S3_SCAN_RESTORE_STARTED")
            restore_wifis3_scan(args)
            print("RA4_WIFI_S3_SCAN_RESTORE_PASS")
        except BaseException as restore_error:
            print("RA4_WIFI_S3_SCAN_RESTORE_FAILED", file=sys.stderr)
            if primary_error is None:
                primary_error = restore_error

    if primary_error is not None:
        raise primary_error
    print("RA4_ARDUINOBLE_BEGIN_TRACE_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
