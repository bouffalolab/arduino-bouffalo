#!/usr/bin/env python3
"""Compile, upload, and validate the paired-board RA4M1 ArduinoBLE begin smoke."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_SKETCH = ROOT_DIR / "examples" / "ra4-arduino-ble-begin-smoke"
DEFAULT_BUILD_DIR = ROOT_DIR / "build" / "ra4-arduino-ble-begin-smoke"
DEFAULT_IMAGE = DEFAULT_BUILD_DIR / "ra4-arduino-ble-begin-smoke.ino.bin"
DEFAULT_BOSSAC_RUNNER = ROOT_DIR / "tools" / "run_ra4_bossac_direct.py"
DEFAULT_RESET_RUNNER = ROOT_DIR / "tools" / "reset_ra4_samba_direct.py"
DEFAULT_FQBN = "arduino:renesas_uno:unor4wifi"


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise RuntimeError(f"command failed with exit status {result.returncode}")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Compile, upload, and validate the RA4M1 ArduinoBLE begin smoke."
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
    parser.add_argument("--capture-ms", type=int, default=8000)
    parser.add_argument("--upload-runtime-settle-ms", type=int, default=150)
    parser.add_argument(
        "--post-upload-settle-ms",
        type=int,
        default=2000,
        help="Wait for the just-reset RA4M1 application to stabilize before reset-only capture.",
    )
    parser.add_argument(
        "--skip-upload",
        action="store_true",
        help="Validate the existing RA4M1 ArduinoBLE smoke without replacing Flash.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.capture_ms <= 0
        or args.upload_runtime_settle_ms < 0
        or args.post_upload_settle_ms < 0
    ):
        raise RuntimeError("capture timeout must be positive and settle time non-negative")

    print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Runtime route: direct BL616CL GPIO34/GPIO35 with RTS=False (relay released).")
    print("Privacy: only fixed RA4_BLE_SMOKE markers and elapsed time are captured.")

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
                str(DEFAULT_BOSSAC_RUNNER),
                "--bridge-port",
                args.bridge_port,
                "--relay-port",
                args.relay_port,
                "--image",
                str(image),
                "--runtime-settle-ms",
                str(args.upload_runtime_settle_ms),
                "--skip-runtime-smoke",
            ]
        )
        if args.post_upload_settle_ms:
            print(f"POST_UPLOAD_SETTLE_MS={args.post_upload_settle_ms}")
            time.sleep(args.post_upload_settle_ms / 1000)

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
            "RA4_BLE_SMOKE:BOOT",
            "--expect",
            "RA4_BLE_SMOKE:BLE_BEGIN=PASS",
            "--expect",
            "RA4_BLE_SMOKE:BLE_END_CALLED",
            "--expect",
            "RA4_BLE_SMOKE:DONE",
            "--expect",
            "RA4_BLE_SMOKE:RESULT=PASS",
        ]
    )
    print("RA4_ARDUINOBLE_BEGIN_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
