#!/usr/bin/env python3
"""Compile, upload, and validate BL616CL SoftSE through the final carrier.

The runner flashes the RA4M1 fixture only. It validates the public
SoftwareATSE API, then resets BL616CL without rewriting its Flash and verifies
that the EasyFlash-backed P-256 key and raw slot remain usable. The standard
WiFiS3 scan fixture is restored on every exit path.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial

from recover_bl616cl_final_carrier_usb import require_bridge_identity
from run_ra4_bossac_final_carrier import require_final_carrier_bridge
from run_ra4_wifis3_scan_smoke import capture_runtime
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_FQBN = "arduino:renesas_uno:unor4wifi"
DEFAULT_UPLOADER = ROOT_DIR / "tools" / "run_ra4_bossac_final_carrier.py"
DEFAULT_RECOVER = ROOT_DIR / "tools" / "recover_bl616cl_final_carrier_usb.py"
DEFAULT_SCAN_RESTORE = ROOT_DIR / "tools" / "run_ra4_wifis3_scan_final_carrier.py"
DEFAULT_SMOKE_SKETCH = ROOT_DIR / "examples" / "ra4-softse-smoke"
DEFAULT_WRITE_SKETCH = ROOT_DIR / "examples" / "ra4-softse-persistence-write"
DEFAULT_READ_SKETCH = ROOT_DIR / "examples" / "ra4-softse-persistence-read"


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def compile_sketch(
    arduino_cli: str,
    fqbn: str,
    sketch: Path,
    build_dir: Path,
) -> Path:
    run_command(
        [
            arduino_cli,
            "compile",
            "--fqbn",
            fqbn,
            "--build-path",
            str(build_dir),
            str(sketch),
        ]
    )
    image = build_dir / f"{sketch.name}.ino.bin"
    if not image.is_file():
        raise RuntimeError(f"compiled RA4M1 image does not exist: {image}")
    return image


def upload(bridge_port: str, image: Path, runtime_settle_ms: int) -> None:
    run_command(
        [
            sys.executable,
            str(DEFAULT_UPLOADER),
            "--bridge-port",
            bridge_port,
            "--image",
            str(image),
            "--runtime-settle-ms",
            str(runtime_settle_ms),
        ]
    )


def require_marker(
    bridge_port: str,
    marker: str,
    failure_prefix: str,
    timeout_seconds: float,
) -> None:
    with serial.Serial(
        bridge_port,
        baudrate=115200,
        timeout=0.1,
        write_timeout=1.0,
    ) as bridge:
        bridge.dtr = False
        bridge.rts = False
        bridge.reset_input_buffer()
        runtime = capture_runtime(bridge, timeout_seconds, 8192)
    if failure_prefix in runtime:
        raise RuntimeError(f"RA4M1 fixture reported failure: {runtime!r}")
    if marker not in runtime:
        raise RuntimeError(f"RA4M1 fixture marker was incomplete: {runtime!r}")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--ftdi-port", default="/dev/ttyUSB0")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--fqbn", default=DEFAULT_FQBN)
    parser.add_argument("--runtime-settle-ms", type=int, default=1000)
    parser.add_argument("--capture-timeout-ms", type=int, default=30000)
    parser.add_argument("--skip-persistence", action="store_true")
    parser.add_argument("--windows-ftdi-reset", action="store_true",
                        help="Reset the verified dedicated BL616CL through the Windows-owned FTDI.")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if args.runtime_settle_ms < 0 or args.capture_timeout_ms <= 0:
        raise RuntimeError("invalid timeout")
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(args.bridge_port)

    print(f"Bridge: {args.bridge_port} (final-carrier BL616CL 2341:1002)")
    print("Target: RA4M1 application Flash only; BL616CL Flash is not rewritten.")
    print("Cleanup: RA4M1 will be restored to WiFiS3 scan smoke.")

    primary_error: BaseException | None = None
    try:
        smoke_image = compile_sketch(
            args.arduino_cli,
            args.fqbn,
            DEFAULT_SMOKE_SKETCH,
            ROOT_DIR / "build" / "ra4-softse-smoke-final-carrier",
        )
        upload(args.bridge_port, smoke_image, args.runtime_settle_ms)
        require_marker(
            args.bridge_port,
            "RA4_SOFTSE:RESULT=PASS",
            "RA4_SOFTSE:FAIL:",
            args.capture_timeout_ms / 1000.0,
        )
        print("RA4_SOFTSE_FINAL_CARRIER_SMOKE_PASS")

        if not args.skip_persistence:
            write_image = compile_sketch(
                args.arduino_cli,
                args.fqbn,
                DEFAULT_WRITE_SKETCH,
                ROOT_DIR / "build" / "ra4-softse-persistence-write-final-carrier",
            )
            upload(args.bridge_port, write_image, args.runtime_settle_ms)
            require_marker(
                args.bridge_port,
                "RA4_SOFTSE_PERSIST_WRITE:PASS",
                "RA4_SOFTSE_PERSIST_WRITE:FAIL:",
                args.capture_timeout_ms / 1000.0,
            )
            print("RA4_SOFTSE_PERSIST_WRITE_PASS")

            if args.windows_ftdi_reset:
                from run_bl616cl_generic_at_final_carrier import restore_usb
                restore_usb(physical_reset=True)
            else:
                run_command(
                    [
                        sys.executable,
                        str(DEFAULT_RECOVER),
                        "--bridge-port",
                        args.bridge_port,
                        "--ftdi-port",
                        args.ftdi_port,
                    ]
                )
            require_bridge_identity(args.bridge_port)
            time.sleep(args.runtime_settle_ms / 1000.0)
            print("BL616CL_SOFTSE_PERSIST_RESET_PASS")

            read_image = compile_sketch(
                args.arduino_cli,
                args.fqbn,
                DEFAULT_READ_SKETCH,
                ROOT_DIR / "build" / "ra4-softse-persistence-read-final-carrier",
            )
            upload(args.bridge_port, read_image, args.runtime_settle_ms)
            require_marker(
                args.bridge_port,
                "RA4_SOFTSE_PERSIST_READ:PASS",
                "RA4_SOFTSE_PERSIST_READ:FAIL:",
                args.capture_timeout_ms / 1000.0,
            )
            print("RA4_SOFTSE_PERSIST_FINAL_CARRIER_PASS")
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
    finally:
        try:
            run_command(
                [
                    sys.executable,
                    str(DEFAULT_SCAN_RESTORE),
                    "--bridge-port",
                    args.bridge_port,
                    "--arduino-cli",
                    args.arduino_cli,
                ]
            )
            print("RA4_SOFTSE_SCAN_RESTORE_PASS")
        except (
            OSError,
            RuntimeError,
            serial.SerialException,
            subprocess.SubprocessError,
        ) as restore_error:
            if primary_error is None:
                primary_error = restore_error
            else:
                print("RA4_SOFTSE_SCAN_RESTORE_FAILED", file=sys.stderr)

    if primary_error is not None:
        raise primary_error
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
