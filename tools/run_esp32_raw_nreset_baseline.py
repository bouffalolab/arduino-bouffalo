#!/usr/bin/env python3
"""Pulse raw CMSIS-DAP nRESET on the ESP32-S3 baseline and observe RA4M1 UART."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial

from run_esp32_swd_d4_baseline import (
    BASELINE_SERIAL,
    discover_windows_bus_id,
    reattach_to_wsl,
    require_baseline_port,
    resolve_wsl_usbip_host,
)
from uno_r4_usb_identity import resolve_baseline_esp32_port


ROOT_DIR = Path(__file__).resolve().parents[1]
WINDOWS_WSL_ROOT = r"\\wsl.localhost\Ubuntu-22.04"
WINDOWS_RUNNER = ROOT_DIR / "tools" / "verify_esp32_raw_nreset_windows.ps1"
WINDOWS_SOURCE = ROOT_DIR / "tools" / "verify_bl616cl_cmsis_dap_windows.cs"


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--wsl-usbip-host", default="auto")
    parser.add_argument("--capture-ms", type=int, default=15000)
    parser.add_argument("--maximum-capture-bytes", type=int, default=8192)
    return parser.parse_args()


def run_windows_raw_reset(bus_id: str) -> None:
    for required in (WINDOWS_RUNNER, WINDOWS_SOURCE):
        if not required.is_file():
            raise RuntimeError(f"required Windows raw-reset file does not exist: {required}")

    runner = WINDOWS_WSL_ROOT + str(WINDOWS_RUNNER)
    source = WINDOWS_WSL_ROOT + str(WINDOWS_SOURCE)
    script = (
        "$ErrorActionPreference = 'Stop'; "
        "$stage = Join-Path $env:TEMP ('uno-r4-esp32-raw-reset-' + "
        "[Guid]::NewGuid().ToString('N')); "
        "New-Item -ItemType Directory -Path $stage | Out-Null; "
        "try { "
        f"Copy-Item -LiteralPath '{runner}' -Destination "
        "(Join-Path $stage 'verify_esp32_raw_nreset_windows.ps1') -Force; "
        f"Copy-Item -LiteralPath '{source}' -Destination "
        "(Join-Path $stage 'verify_bl616cl_cmsis_dap_windows.cs') -Force; "
        f"$busid = '{bus_id}'; "
        "usbipd wsl detach --busid $busid; "
        "Start-Sleep -Milliseconds 750; "
        "& (Join-Path $stage 'verify_esp32_raw_nreset_windows.ps1'); "
        "if ($LASTEXITCODE -ne 0) { "
        "throw \"ESP32-S3 baseline raw-reset helper failed with exit code $LASTEXITCODE\" "
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
        raise RuntimeError(
            f"ESP32-S3 baseline Windows raw nRESET failed with exit code {result.returncode}"
        )


def capture_runtime(port: str, timeout_seconds: float, maximum_bytes: int) -> str:
    link = serial.Serial(
        port=None,
        baudrate=115200,
        timeout=0.1,
        write_timeout=1.0,
    )
    link.dtr = True
    link.rts = False
    link.port = port
    link.open()
    try:
        link.reset_input_buffer()
        deadline = time.monotonic() + timeout_seconds
        received = bytearray()
        while time.monotonic() < deadline:
            payload = link.read(512)
            if payload:
                received.extend(payload)
                if len(received) > maximum_bytes:
                    raise RuntimeError("baseline UART capture exceeded configured limit")
        return received.decode("ascii", errors="replace").replace("\x00", "")
    finally:
        link.close()


def main() -> int:
    args = parse_arguments()
    if args.capture_ms <= 0 or args.maximum_capture_bytes <= 0:
        raise RuntimeError("capture limits must be positive")

    port = resolve_baseline_esp32_port(args.bridge_port)
    require_baseline_port(port)
    host = resolve_wsl_usbip_host(args.wsl_usbip_host)
    bus_id = discover_windows_bus_id()
    print(f"ESP32_BASELINE_PORT={port} serial={BASELINE_SERIAL}")
    print("Target: RA4M1 raw nRESET and user UART only; no Flash is written.")

    try:
        run_windows_raw_reset(bus_id)
    finally:
        port = reattach_to_wsl(host, bus_id, 10.0)

    runtime = capture_runtime(
        port,
        args.capture_ms / 1000.0,
        args.maximum_capture_bytes,
    )
    print("ESP32_RAW_NRESET_RUNTIME_CAPTURE_BEGIN")
    print(runtime, end="" if runtime.endswith("\n") else "\n")
    print("ESP32_RAW_NRESET_RUNTIME_CAPTURE_END")
    print(f"ESP32_RAW_NRESET_RUNTIME_BYTES={len(runtime.encode('ascii', 'replace'))}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
