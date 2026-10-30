#!/usr/bin/env python3
"""Validate WiFiS3.getTime() through the final BL616CL carrier.

The runner loads the private STA credentials only into process memory. It
temporarily replaces the RA4M1 Sketch and restores the standard WiFiS3 scan
Sketch in all exit paths; the BL616CL application Flash is not rewritten.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_bl616cl_apsta_gate import DEFAULT_CREDENTIAL_ENV, load_private_credentials
from run_ra4_bossac_final_carrier import require_final_carrier_bridge
import run_ra4_wifis3_scan_final_carrier as final_scan
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT_DIR = Path(__file__).resolve().parents[1]
FQBN = "arduino:renesas_uno:unor4wifi"
SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-gettime-smoke"
BUILD_DIR = ROOT_DIR / "build" / "ra4-wifis3-gettime-smoke"
IMAGE = BUILD_DIR / "ra4-wifis3-gettime-smoke.ino.bin"
SAMBA_RUNNER = ROOT_DIR / "tools" / "run_ra4_bossac_final_carrier.py"
MINIMUM_EPOCH = 946684800


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def encode_start_value(value: str) -> str:
    return value.encode("ascii", "strict").hex()


def compile_sketch(args: argparse.Namespace) -> Path:
    run_command(
        [
            args.arduino_cli,
            "compile",
            "--fqbn",
            FQBN,
            "--build-path",
            str(BUILD_DIR),
            str(SKETCH),
        ]
    )
    if not IMAGE.is_file():
        raise RuntimeError("compiled WiFiS3 getTime Sketch image does not exist")
    return IMAGE


def upload_sketch(args: argparse.Namespace, image: Path) -> None:
    run_command(
        [
            sys.executable,
            str(SAMBA_RUNNER),
            "--bridge-port",
            args.bridge_port,
            "--image",
            str(image),
            "--runtime-settle-ms",
            str(args.runtime_settle_ms),
            "--attempts",
            "3",
        ]
    )


def restore_scan(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(final_scan.__file__),
            "--bridge-port",
            args.bridge_port,
            "--arduino-cli",
            args.arduino_cli,
        ]
    )
    print("RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_PASS")


def wait_for_markers(
    bridge: serial.Serial,
    markers: tuple[str, ...],
    timeout_seconds: float,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        payload = bridge.read(512)
        if not payload:
            continue
        received.extend(payload)
        if len(received) > 8192:
            raise RuntimeError("RA4M1 getTime output exceeded its byte limit")
        text = received.decode("ascii", errors="replace").replace("\x00", "")
        if any(marker in text for marker in markers):
            return text
    raise RuntimeError("RA4M1 getTime Sketch did not produce an expected marker")


def print_markers(text: str) -> None:
    for line in text.replace("\x00", "").splitlines():
        if line.strip().startswith("RA4_GETTIME:"):
            print(line.strip())


def parse_epoch(text: str) -> int:
    for line in text.replace("\x00", "").splitlines():
        if line.startswith("RA4_GETTIME:EPOCH="):
            try:
                return int(line.split("=", 1)[1], 10)
            except ValueError as error:
                raise RuntimeError("RA4M1 getTime epoch is malformed") from error
    raise RuntimeError("RA4M1 getTime output lacks its epoch marker")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--ready-timeout-ms", type=int, default=15000)
    parser.add_argument("--runtime-timeout-ms", type=int, default=60000)
    parser.add_argument("--runtime-settle-ms", type=int, default=1000)
    parser.add_argument(
        "--source-only",
        action="store_true",
        help="Compile the RA4M1 getTime Sketch without using hardware.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.ready_timeout_ms <= 0
        or args.runtime_timeout_ms <= 0
        or args.runtime_settle_ms < 0
    ):
        raise RuntimeError("invalid timeout")

    if args.source_only:
        compile_sketch(args)
        print("RA4_WIFIS3_GETTIME_SOURCE_PASS")
        return 0

    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(args.bridge_port)
    ssid, password = load_private_credentials(args.credential_env)
    if password is None:
        raise RuntimeError("WiFiS3 getTime gate requires a private WPA password")

    print(f"Bridge: {args.bridge_port} (final-carrier BL616CL 2341:1002)")
    print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Privacy: STA credentials remain in process memory.")

    bridge = None
    ra4_modified = False
    primary_error: Exception | None = None
    try:
        image = compile_sketch(args)
        upload_sketch(args, image)
        ra4_modified = True
        bridge = serial.Serial(
            args.bridge_port,
            baudrate=115200,
            timeout=0.1,
            write_timeout=1.0,
        )
        bridge.dtr = False
        bridge.rts = False
        bridge.reset_input_buffer()
        startup = wait_for_markers(
            bridge,
            ("RA4_GETTIME:READY",),
            args.ready_timeout_ms / 1000.0,
        )
        print_markers(startup)
        bridge.reset_input_buffer()
        bridge.write(
            (
                "START "
                + encode_start_value(ssid)
                + " "
                + encode_start_value(password)
                + "\n"
            ).encode("ascii")
        )
        bridge.flush()
        runtime = wait_for_markers(
            bridge,
            ("RA4_GETTIME:RESULT=PASS", "RA4_GETTIME:RESULT=FAIL:"),
            args.runtime_timeout_ms / 1000.0,
        )
        print_markers(runtime)
        if "RA4_GETTIME:RESULT=PASS" not in runtime:
            raise RuntimeError("RA4M1 WiFiS3 getTime Sketch reported failure")
        epoch = parse_epoch(runtime)
        if epoch < MINIMUM_EPOCH:
            raise RuntimeError(f"RA4M1 WiFiS3 getTime epoch is pre-2000: {epoch}")
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
    finally:
        if bridge is not None and bridge.is_open:
            bridge.close()
        if ra4_modified:
            try:
                restore_scan(args)
            except (
                OSError,
                RuntimeError,
                serial.SerialException,
                subprocess.SubprocessError,
            ):
                print("RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_FAILED", file=sys.stderr)
                if primary_error is None:
                    primary_error = RuntimeError("RA4M1 scan restore failed")

    if primary_error is not None:
        raise primary_error
    print("RA4_WIFIS3_GETTIME_GATE_PASS")
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
