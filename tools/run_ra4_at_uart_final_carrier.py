#!/usr/bin/env python3
"""Validate RA4M1 AT UART forwarding on the final BL616CL carrier."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial

from run_ra4_at_uart_gate import REQUESTS, transact_exact_query
from uno_r4_usb_identity import resolve_paired_bl616cl_port, tty_properties


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_FQBN = "arduino:renesas_uno:unor4wifi"
DEFAULT_SKETCH = ROOT_DIR / "examples" / "ra4-at-transcript-proxy"
DEFAULT_BUILD_DIR = ROOT_DIR / "build" / "ra4-at-transcript-proxy-final-carrier"
DEFAULT_IMAGE = DEFAULT_BUILD_DIR / "ra4-at-transcript-proxy.ino.bin"
DEFAULT_UPLOADER = ROOT_DIR / "tools" / "run_ra4_bossac_final_carrier.py"
DEFAULT_SCAN_RESTORE = ROOT_DIR / "tools" / "run_ra4_wifis3_scan_final_carrier.py"
DEFAULT_STATUS_RESPONSE = b"+GETSTATUS: 2\r\nOK\r\n"


def require_final_carrier_bridge(port: str) -> None:
    properties = tty_properties(port)
    if (
        properties.get("ID_VENDOR_ID", "").lower() != "2341"
        or properties.get("ID_MODEL_ID", "").lower() != "1002"
        or properties.get("ID_USB_INTERFACE_NUM", "").lower() != "00"
        or properties.get("ID_SERIAL_SHORT") != "BL616CL-PROVISIONAL"
    ):
        raise RuntimeError(f"{port} is not the final-carrier BL616CL CDC interface")


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def open_bridge(port: str, timeout_seconds: float) -> serial.Serial:
    link = serial.Serial(
        port,
        baudrate=115200,
        timeout=timeout_seconds,
        write_timeout=timeout_seconds,
    )
    link.dtr = False
    link.rts = False
    return link


def validate_proxy(
    port: str,
    timeout_seconds: float,
    include_status: bool,
    expected_status_response: bytes,
) -> None:
    with open_bridge(port, timeout_seconds) as bridge:
        bridge.reset_input_buffer()
        requests = REQUESTS[:2]
        if include_status:
            requests += ((b"AT+GETSTATUS?\r\n", expected_status_response),)
        for request, expected in requests:
            actual = transact_exact_query(
                bridge,
                bridge,
                request,
                expected,
                timeout_seconds,
            )
            print(f"AT_REQUEST={request!r}")
            print(f"AT_RESPONSE={actual!r}")
            if actual != expected:
                raise RuntimeError(
                    f"unexpected response for {request!r}: "
                    f"expected {expected!r}, got {actual!r}"
                )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--fqbn", default=DEFAULT_FQBN)
    parser.add_argument("--sketch", type=Path, default=DEFAULT_SKETCH)
    parser.add_argument("--build-dir", type=Path, default=DEFAULT_BUILD_DIR)
    parser.add_argument("--image", type=Path, default=DEFAULT_IMAGE)
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--runtime-settle-ms", type=int, default=1000)
    parser.add_argument(
        "--expected-status",
        type=int,
        default=2,
        help=(
            "Expected decimal GETSTATUS value. The default is scan-completed "
            "because this runner restores and starts from the WiFiS3 scan gate."
        ),
    )
    parser.add_argument("--skip-upload", action="store_true")
    parser.add_argument("--skip-status-check", action="store_true")
    parser.add_argument(
        "--keep-proxy",
        action="store_true",
        help="Leave the temporary RA4M1 AT proxy installed after validation.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(args.bridge_port)
    if (
        args.response_timeout_ms <= 0
        or args.runtime_settle_ms < 0
        or args.expected_status < 0
        or args.expected_status > 255
    ):
        raise RuntimeError("invalid timeout")

    print(f"Bridge: {args.bridge_port} (final-carrier BL616CL 2341:1002)")
    print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Transport: one CDC stream through RA4M1 Serial <-> Serial2 AT proxy.")
    uploaded_proxy = False
    primary_error: Exception | None = None
    try:
        if not args.skip_upload:
            sketch = args.sketch.resolve()
            build_dir = args.build_dir.resolve()
            image = args.image.resolve()
            if not sketch.is_dir():
                raise RuntimeError(f"RA4M1 proxy Sketch does not exist: {sketch}")
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
                raise RuntimeError(f"compiled proxy image does not exist: {image}")
            run_command(
                [
                    sys.executable,
                    str(DEFAULT_UPLOADER),
                    "--bridge-port",
                    args.bridge_port,
                    "--image",
                    str(image),
                    "--runtime-settle-ms",
                    str(args.runtime_settle_ms),
                ]
            )
            uploaded_proxy = True
        time.sleep(args.runtime_settle_ms / 1000.0)
        validate_proxy(
            args.bridge_port,
            args.response_timeout_ms / 1000.0,
            not args.skip_status_check,
            (
                f"+GETSTATUS: {args.expected_status}\r\nOK\r\n"
            ).encode("ascii"),
        )
        print("RA4_AT_UART_FINAL_CARRIER_PASS")
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
    finally:
        if uploaded_proxy and not args.keep_proxy:
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
                print("RA4_SCAN_FINAL_CARRIER_RESTORE_PASS")
            except (
                OSError,
                RuntimeError,
                serial.SerialException,
                subprocess.SubprocessError,
            ) as restore_error:
                if primary_error is None:
                    primary_error = restore_error
                else:
                    print(
                        "RA4_SCAN_FINAL_CARRIER_RESTORE_FAILED",
                        file=sys.stderr,
                    )

    if primary_error is not None:
        raise primary_error
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
