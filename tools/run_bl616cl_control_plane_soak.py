#!/usr/bin/env python3
"""Soak the paired BL616CL CDC/AT and HID control paths without Wi-Fi mutation.

The RA4M1 temporary proxy forwards the host CDC port to the BL616CL AT UART.
Each cycle verifies exact AT and firmware-version frames and reads a CMSIS-DAP
HID feature page. The normal RA4M1 WiFiS3 scan Sketch is restored by default.
"""

from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
import inspect_bl616cl_gpio35 as telemetry
import run_ra4_at_uart_gate as at_gate
from ra4_runtime_uart_route import open_runtime_route
from uno_r4_usb_identity import resolve_paired_bl616cl_port, resolve_relay_port


ROOT_DIR = Path(__file__).resolve().parents[1]
SCAN_RESTORE = ROOT_DIR / "tools" / "run_ra4_wifis3_scan_smoke.py"


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def ensure_hidraw_access(
    prepare_via_wsl_root: bool, wsl_distribution: str
) -> str:
    hidraw = telemetry.find_hidraw()
    if os.access(hidraw, os.R_OK | os.W_OK):
        return hidraw
    if not prepare_via_wsl_root:
        raise RuntimeError(
            f"HID node is not readable and writable: {hidraw}; rerun with "
            "--prepare-hidraw-via-wsl-root in this WSL USB/IP environment"
        )

    command = [
        "powershell.exe",
        "-NoProfile",
        "-Command",
        (
            f"wsl.exe -d {wsl_distribution} -u root -- "
            f"chmod a+rw {hidraw}"
        ),
    ]
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError(f"could not grant temporary access to {hidraw}")
    if not os.access(hidraw, os.R_OK | os.W_OK):
        raise RuntimeError(f"HID node is still not readable and writable: {hidraw}")
    return hidraw


def read_hid_control_page(hidraw: str) -> None:
    fd = os.open(hidraw, os.O_RDWR | os.O_NONBLOCK)
    try:
        telemetry.select_feature_page(fd, 1)
        report = telemetry.read_feature(fd)
    finally:
        try:
            telemetry.select_feature_page(fd, 0)
        except OSError:
            pass
        os.close(fd)

    if report[:5] != b"U35D" + bytes((telemetry.FEATURE_VERSION,)):
        raise RuntimeError("unexpected BL616CL HID feature signature")
    if report[5] != 1:
        raise RuntimeError("unexpected BL616CL HID control feature page")


def transact(
    bridge: serial.Serial,
    receiver: serial.Serial,
    request: bytes,
    expected: bytes,
    timeout_seconds: float,
) -> None:
    actual = at_gate.transact_exact_query(
        bridge, receiver, request, expected, timeout_seconds
    )
    if actual != expected:
        raise RuntimeError(
            f"unexpected response for {request!r}: expected {expected!r}, got {actual!r}"
        )


def restore_scan(args: argparse.Namespace) -> None:
    run_command(
        [
            sys.executable,
            str(SCAN_RESTORE),
            "--bridge-port",
            args.bridge_port,
            "--relay-port",
            args.relay_port,
            "--arduino-cli",
            args.arduino_cli,
        ]
    )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--duration-ms", type=int, default=30000)
    parser.add_argument("--interval-ms", type=int, default=250)
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--prepare-hidraw-via-wsl-root", action="store_true")
    parser.add_argument("--wsl-distribution", default="Ubuntu-22.04")
    parser.add_argument(
        "--keep-proxy",
        action="store_true",
        help="Leave the temporary RA4M1 AT proxy installed after the soak.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    if (
        args.duration_ms <= 0
        or args.interval_ms < 0
        or args.response_timeout_ms <= 0
    ):
        raise RuntimeError("duration and timeouts must be positive")

    at_gate.require_port_identity(args.bridge_port, "2341", "1002", "00")
    at_gate.require_port_identity(args.relay_port, "0403", "6010", "00")
    print(f"Bridge: {args.bridge_port} (2341:1002 paired BL616CL)")
    print(f"Relay:  {args.relay_port} (0403:6010 interface 00)")
    print("Target: paired RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Scope: no Wi-Fi connect, scan, or SoftAP request is sent.")

    proxy_uploaded = False
    primary_error: Exception | None = None
    try:
        run_command(
            [
                sys.executable,
                str(ROOT_DIR / "tools" / "run_ra4_at_uart_gate.py"),
                "--bridge-port",
                args.bridge_port,
                "--relay-port",
                args.relay_port,
                "--arduino-cli",
                args.arduino_cli,
                "--skip-status-check",
            ]
        )
        proxy_uploaded = True

        route = open_runtime_route(
            args.bridge_port,
            args.relay_port,
            bridge_timeout=1.0,
            receiver_timeout=0.05,
        )
        hidraw = ensure_hidraw_access(
            args.prepare_hidraw_via_wsl_root, args.wsl_distribution
        )
        cycles = 0
        started_at = time.monotonic()
        try:
            deadline = started_at + args.duration_ms / 1000.0
            while time.monotonic() < deadline:
                transact(
                    route.bridge,
                    route.receiver,
                    b"AT\r\n",
                    b"OK\r\n",
                    args.response_timeout_ms / 1000.0,
                )
                transact(
                    route.bridge,
                    route.receiver,
                    b"AT+FWVERSION?\r\n",
                    b"+FWVERSION: 0.6.0\r\nOK\r\n",
                    args.response_timeout_ms / 1000.0,
                )
                read_hid_control_page(hidraw)
                cycles += 1
                if cycles % 10 == 0:
                    print(f"CONTROL_PLANE_SOAK_PROGRESS cycles={cycles}")
                time.sleep(args.interval_ms / 1000.0)
        finally:
            route.close()

        elapsed_ms = (time.monotonic() - started_at) * 1000.0
        print(f"CONTROL_PLANE_SOAK_CYCLES={cycles}")
        print(f"CONTROL_PLANE_SOAK_ELAPSED_MS={elapsed_ms:.3f}")
        print("BL616CL_CONTROL_PLANE_SOAK_PASS")
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
    finally:
        if proxy_uploaded and not args.keep_proxy:
            try:
                restore_scan(args)
            except (
                OSError,
                RuntimeError,
                serial.SerialException,
                subprocess.SubprocessError,
            ) as error:
                if primary_error is None:
                    primary_error = error
                else:
                    print("RA4_SCAN_RESTORE_FAILED", file=sys.stderr)

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
