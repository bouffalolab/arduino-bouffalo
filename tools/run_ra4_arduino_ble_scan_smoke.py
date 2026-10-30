#!/usr/bin/env python3
"""Run the paired RA4M1 ArduinoBLE central-scan smoke and restore WiFiS3 scan."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_SKETCH = ROOT_DIR / "examples" / "ra4-arduino-ble-scan-smoke"
DEFAULT_BUILD_DIR = ROOT_DIR / "build" / "ra4-arduino-ble-scan-smoke"
DEFAULT_IMAGE = DEFAULT_BUILD_DIR / "ra4-arduino-ble-scan-smoke.ino.bin"
DEFAULT_BOSSAC_RUNNER = ROOT_DIR / "tools" / "run_ra4_bossac_direct.py"
DEFAULT_RESET_RUNNER = ROOT_DIR / "tools" / "reset_ra4_samba_direct.py"
DEFAULT_RESTORE_RUNNER = ROOT_DIR / "tools" / "run_ra4_wifis3_scan_smoke.py"
DEFAULT_FQBN = "arduino:renesas_uno:unor4wifi"

REPORT_COUNT_PATTERN = re.compile(r"RA4_BLE_SCAN:REPORT_COUNT=(\d+)")


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"command failed with exit status {result.returncode}")


def run_scan_capture(args: argparse.Namespace) -> int:
    reset_runner = (ROOT_DIR / "tools/reset_ra4_samba_final_carrier.py"
                    if args.final_carrier else DEFAULT_RESET_RUNNER)
    command = [
        sys.executable,
        str(reset_runner),
        "--bridge-port",
        args.bridge_port,
        "--capture-ms",
        str(args.capture_ms),
        "--stop-when-expectations-met",
        "--expect",
        "RA4_BLE_SCAN:BOOT",
        "--expect",
        "RA4_BLE_SCAN:BLE_BEGIN=PASS",
        "--expect",
        "RA4_BLE_SCAN:SCAN_START=PASS",
        "--expect",
        "RA4_BLE_SCAN:SCAN_STOP_CALLED",
        "--expect",
        "RA4_BLE_SCAN:BLE_END_CALLED",
        "--expect",
        "RA4_BLE_SCAN:DONE",
        "--expect",
        "RA4_BLE_SCAN:RESULT=PASS",
    ]
    if not args.final_carrier:
        command.extend(["--relay-port", args.relay_port])
    print("COMMAND=" + " ".join(command))
    result = subprocess.run(command, check=False, capture_output=True, text=True)
    if result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    if result.stderr:
        print(result.stderr, end="" if result.stderr.endswith("\n") else "\n", file=sys.stderr)
    if result.returncode != 0:
        raise RuntimeError(f"scan runtime check failed with exit status {result.returncode}")

    report_count = REPORT_COUNT_PATTERN.search(result.stdout)
    if report_count is None:
        raise RuntimeError("scan runtime did not report a sanitized advertising-report count")
    count = int(report_count.group(1))
    if count == 0:
        raise RuntimeError("scan runtime did not observe any advertising reports")
    return count


def restore_wifis3_scan(args: argparse.Namespace) -> None:
    if args.final_carrier:
        run_command([
            sys.executable,
            str(ROOT_DIR / "tools/run_ra4_wifis3_scan_final_carrier.py"),
            "--bridge-port", args.bridge_port,
            "--arduino-cli", args.arduino_cli,
        ])
        return
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
        description="Compile, run, and restore the paired RA4M1 ArduinoBLE central-scan smoke."
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
    parser.add_argument("--fqbn", default=DEFAULT_FQBN)
    parser.add_argument("--sketch", type=Path, default=DEFAULT_SKETCH)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    parser.add_argument("--image", type=Path, default=DEFAULT_IMAGE)
    parser.add_argument("--capture-ms", type=int, default=16000)
    parser.add_argument("--upload-runtime-settle-ms", type=int, default=150)
    parser.add_argument("--post-upload-settle-ms", type=int, default=2000)
    parser.add_argument("--usbipd-busid")
    parser.add_argument("--usbipd-distribution", default="Ubuntu-22.04")
    parser.add_argument("--relay-already-runtime", action="store_true")
    parser.add_argument(
        "--final-carrier", action="store_true",
        help="Use the current carrier's CDC-only upload, reset and restoration paths; do not open FTDI.",
    )
    parser.add_argument(
        "--skip-upload",
        action="store_true",
        help="Validate an already-running scan Sketch without replacing or restoring RA4M1 Flash.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.capture_ms <= 0
        or args.upload_runtime_settle_ms < 0
        or args.post_upload_settle_ms < 0
    ):
        raise RuntimeError("capture timeout must be positive and settle times non-negative")

    print("Target: paired RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Runtime route: direct BL616CL GPIO34/GPIO35"
          + (" (CDC only; FTDI untouched)." if args.final_carrier
             else " with RTS=False (relay released)."))
    print("Privacy: the Sketch only reports a count, never BLE addresses, names, or payloads.")
    if not args.skip_upload:
        print("Cleanup: WiFiS3 scan Sketch will be restored even if the BLE scan check fails.")

    primary_error: BaseException | None = None
    restore_required = not args.skip_upload
    try:
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
                    "--libraries",
                    str(ROOT_DIR / "libraries"),
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
                    str(ROOT_DIR / "tools/run_ra4_bossac_final_carrier.py"
                        if args.final_carrier else DEFAULT_BOSSAC_RUNNER),
                    "--bridge-port",
                    args.bridge_port,
                    "--image",
                    str(image),
                    "--runtime-settle-ms",
                    str(args.upload_runtime_settle_ms),
                ]
                + ([] if args.final_carrier else
                   ["--relay-port", args.relay_port, "--skip-runtime-smoke"])
                + (["--usbipd-busid", args.usbipd_busid,
                    "--usbipd-distribution", args.usbipd_distribution]
                   if args.usbipd_busid and not args.final_carrier else [])
                + (["--relay-already-runtime"]
                   if args.relay_already_runtime and not args.final_carrier else [])
            )
            if args.post_upload_settle_ms:
                print(f"POST_UPLOAD_SETTLE_MS={args.post_upload_settle_ms}")
                time.sleep(args.post_upload_settle_ms / 1000)

        report_count = run_scan_capture(args)
        print(f"RA4_BLE_SCAN_REPORT_COUNT={report_count}")
        print("RA4_ARDUINOBLE_SCAN_PASS")
    except BaseException as error:
        primary_error = error
    finally:
        if restore_required:
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
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
