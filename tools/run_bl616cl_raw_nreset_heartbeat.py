#!/usr/bin/env python3
"""Pulse final-carrier raw nRESET and require the RA4M1 heartbeat to restart."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

import serial

from read_bl616cl_windows_telemetry import (
    discover_windows_bus_id,
    reattach_bridge,
    resolve_wsl_usbip_host,
)
from uno_r4_usb_identity import resolve_paired_bl616cl_port, tty_properties


ROOT_DIR = Path(__file__).resolve().parents[1]
WINDOWS_WSL_ROOT = r"\\wsl.localhost\Ubuntu-22.04"
WINDOWS_RUNNER = ROOT_DIR / "tools" / "verify_bl616cl_raw_nreset_windows.ps1"
WINDOWS_SOURCE = ROOT_DIR / "tools" / "verify_bl616cl_cmsis_dap_windows.cs"
BOOT_MARKER = "RA4_HEARTBEAT:BOOT"
HEARTBEAT_PATTERN = re.compile(r"^RA4_HEARTBEAT:[0-9]+\r?$", re.MULTILINE)
HEARTBEAT_VALUE_PATTERN = re.compile(r"^RA4_HEARTBEAT:([0-9]+)\r?$", re.MULTILINE)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--capture-ms", type=int, default=8000)
    parser.add_argument("--maximum-capture-bytes", type=int, default=8192)
    parser.add_argument(
        "--skip-preflight",
        action="store_true",
        help="allow a recovery pulse when the current RA4M1 application is not observable",
    )
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


def run_windows_raw_reset(bus_id: str) -> None:
    for required in (WINDOWS_RUNNER, WINDOWS_SOURCE):
        if not required.is_file():
            raise RuntimeError(f"required Windows raw-reset file does not exist: {required}")

    runner = WINDOWS_WSL_ROOT + str(WINDOWS_RUNNER)
    source = WINDOWS_WSL_ROOT + str(WINDOWS_SOURCE)
    script = (
        "$ErrorActionPreference = 'Stop'; "
        "$stage = Join-Path $env:TEMP ('uno-r4-bl616cl-raw-reset-' + "
        "[Guid]::NewGuid().ToString('N')); "
        "New-Item -ItemType Directory -Path $stage | Out-Null; "
        "try { "
        f"Copy-Item -LiteralPath '{runner}' -Destination "
        "(Join-Path $stage 'verify_bl616cl_raw_nreset_windows.ps1') -Force; "
        f"Copy-Item -LiteralPath '{source}' -Destination "
        "(Join-Path $stage 'verify_bl616cl_cmsis_dap_windows.cs') -Force; "
        f"$busid = '{bus_id}'; "
        "usbipd wsl detach --busid $busid; "
        "Start-Sleep -Milliseconds 750; "
        "& (Join-Path $stage 'verify_bl616cl_raw_nreset_windows.ps1'); "
        "if ($LASTEXITCODE -ne 0) { "
        "throw \"BL616CL raw-reset helper failed with exit code $LASTEXITCODE\" "
        "} "
        "} finally { "
        "Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue "
        "}"
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
        check=False,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=60.0,
    )
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        raise RuntimeError(f"Windows raw nRESET helper failed with exit code {result.returncode}")


def capture_heartbeat(port: str, timeout_seconds: float, maximum_bytes: int) -> str:
    link = serial.Serial(port, baudrate=115200, timeout=0.1, write_timeout=1.0)
    try:
        link.dtr = False
        link.rts = False
        link.reset_input_buffer()
        deadline = time.monotonic() + timeout_seconds
        received = bytearray()
        while time.monotonic() < deadline:
            payload = link.read(512)
            if payload:
                received.extend(payload)
                if len(received) > maximum_bytes:
                    raise RuntimeError("heartbeat capture exceeded configured limit")
                text = received.decode("ascii", errors="replace").replace("\x00", "")
                if len(HEARTBEAT_PATTERN.findall(text)) >= 2:
                    return text
        return received.decode("ascii", errors="replace").replace("\x00", "")
    finally:
        link.close()


def last_heartbeat_value(runtime: str) -> int:
    values = [int(value) for value in HEARTBEAT_VALUE_PATTERN.findall(runtime)]
    if not values:
        raise RuntimeError("heartbeat capture did not contain any counter values")
    return values[-1]


def main() -> int:
    args = parse_arguments()
    if args.capture_ms <= 0 or args.maximum_capture_bytes <= 0:
        raise RuntimeError("capture limits must be positive")

    port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier(port)
    host = resolve_wsl_usbip_host("auto")
    bus_id = discover_windows_bus_id()
    print(f"BL616CL_FINAL_CARRIER_PORT={port}")
    print("Target: RA4M1 raw nRESET and user UART only; no Flash is written.")
    before_count: int | None = None
    if not args.skip_preflight:
        before_runtime = capture_heartbeat(port, 3.0, args.maximum_capture_bytes)
        before_count = last_heartbeat_value(before_runtime)
        print(f"BL616CL_RAW_NRESET_HEARTBEAT_BEFORE={before_count}")
    else:
        print("BL616CL_RAW_NRESET_HEARTBEAT_BEFORE=unavailable_recovery_mode")

    try:
        run_windows_raw_reset(bus_id)
    finally:
        reattach_bridge("Ubuntu-22.04", host, bus_id, port, 10.0)
    require_final_carrier(port)

    runtime = capture_heartbeat(
        port,
        args.capture_ms / 1000.0,
        args.maximum_capture_bytes,
    )
    print("BL616CL_RAW_NRESET_HEARTBEAT_CAPTURE_BEGIN")
    print(runtime, end="" if runtime.endswith("\n") else "\n")
    print("BL616CL_RAW_NRESET_HEARTBEAT_CAPTURE_END")
    if len(HEARTBEAT_PATTERN.findall(runtime)) < 2:
        raise RuntimeError("raw nRESET produced fewer than two RA4M1 heartbeat markers")
    after_count = last_heartbeat_value(runtime)
    print(f"BL616CL_RAW_NRESET_HEARTBEAT_AFTER={after_count}")
    if before_count is not None and after_count >= before_count:
        raise RuntimeError(
            "raw nRESET did not restart the heartbeat counter: "
            f"before={before_count} after={after_count}"
        )
    print(
        "BL616CL_RA4_RAW_NRESET_HEARTBEAT_RESTART_PASS"
        if before_count is not None
        else "BL616CL_RA4_RAW_NRESET_HEARTBEAT_RECOVERY_PASS"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
