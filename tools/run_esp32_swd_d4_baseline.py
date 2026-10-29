#!/usr/bin/env python3
"""Run CMSIS-DAP D4 on the registered ESP32-S3 baseline without Flash writes."""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

import serial

from run_ra4_wifis3_scan_smoke import (
    COUNT_PATTERN,
    ELAPSED_PATTERN,
    FAIL_PATTERN,
    FIRMWARE_PATTERN,
    PASS_MARKER,
    capture_runtime,
)
from uno_r4_usb_identity import (
    resolve_baseline_esp32_port,
    tty_properties,
)


ROOT_DIR = Path(__file__).resolve().parents[1]
WINDOWS_WSL_ROOT = r"\\wsl.localhost\Ubuntu-22.04"
BASELINE_SERIAL = "TESTESP000002"
WINDOWS_D4_RUNNER = ROOT_DIR / "tools" / "verify_esp32_swd_halt_reset_windows.ps1"
WINDOWS_D4_SOURCE = ROOT_DIR / "tools" / "verify_bl616cl_cmsis_dap_windows.cs"


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--wsl-usbip-host", default="auto")
    parser.add_argument("--capture-timeout-ms", type=int, default=45000)
    parser.add_argument("--maximum-capture-bytes", type=int, default=8192)
    return parser.parse_args()


def require_baseline_port(port: str) -> None:
    properties = tty_properties(port)
    if (
        properties.get("ID_VENDOR_ID", "").lower() != "2341"
        or properties.get("ID_MODEL_ID", "").lower() != "1002"
        or properties.get("ID_USB_INTERFACE_NUM", "").lower() != "01"
        or properties.get("ID_SERIAL_SHORT") != BASELINE_SERIAL
    ):
        raise RuntimeError(
            f"{port} is not the registered ESP32-S3 baseline {BASELINE_SERIAL}"
        )


def resolve_wsl_usbip_host(requested: str) -> str:
    if requested != "auto":
        return requested
    result = subprocess.run(
        ["ip", "route", "show", "default"],
        check=False,
        capture_output=True,
        text=True,
    )
    fields = result.stdout.split()
    if result.returncode != 0 or len(fields) < 3 or fields[:2] != ["default", "via"]:
        raise RuntimeError("cannot resolve the Windows USB/IP host from the WSL route")
    return fields[2]


def discover_windows_bus_id() -> str:
    script = (
        "$ErrorActionPreference = 'Stop'; "
        "$matches = @((usbipd state | ConvertFrom-Json).Devices | "
        "Where-Object { $_.BusId -ne $null -and "
        "$_.InstanceId.EndsWith('\\"
        + BASELINE_SERIAL
        + "', [System.StringComparison]::OrdinalIgnoreCase) } | "
        "ForEach-Object { $_.BusId }); "
        "if ($matches.Count -ne 1) { "
        "throw \"expected exactly one registered ESP32-S3 baseline USB device, "
        "found $($matches.Count)\" "
        "}; "
        "Write-Output $matches[0]"
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", script],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=30.0,
    )
    if result.returncode != 0 or not result.stdout.strip():
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError("cannot locate registered ESP32-S3 baseline USB device: " + detail)
    return result.stdout.strip()


def run_windows_d4(bus_id: str) -> None:
    for required in (WINDOWS_D4_RUNNER, WINDOWS_D4_SOURCE):
        if not required.is_file():
            raise RuntimeError(f"required Windows D4 file does not exist: {required}")

    windows_runner = WINDOWS_WSL_ROOT + str(WINDOWS_D4_RUNNER)
    windows_source = WINDOWS_WSL_ROOT + str(WINDOWS_D4_SOURCE)
    script = (
        "$ErrorActionPreference = 'Stop'; "
        "$stage = Join-Path $env:TEMP ('uno-r4-esp32-d4-' + "
        "[Guid]::NewGuid().ToString('N')); "
        "New-Item -ItemType Directory -Path $stage | Out-Null; "
        "try { "
        f"Copy-Item -LiteralPath '{windows_runner}' -Destination "
        "(Join-Path $stage 'verify_esp32_swd_halt_reset_windows.ps1') -Force; "
        f"Copy-Item -LiteralPath '{windows_source}' -Destination "
        "(Join-Path $stage 'verify_bl616cl_cmsis_dap_windows.cs') -Force; "
        f"$busid = '{bus_id}'; "
        "usbipd wsl detach --busid $busid; "
        "Start-Sleep -Milliseconds 750; "
        "& (Join-Path $stage 'verify_esp32_swd_halt_reset_windows.ps1'); "
        "if ($LASTEXITCODE -ne 0) { "
        "throw \"ESP32-S3 baseline D4 helper failed with exit code $LASTEXITCODE\" "
        "} "
        "Write-Output \"ESP32_D4_BUSID=$busid\" "
        "} finally { "
        "Remove-Item -LiteralPath $stage -Recurse -Force -ErrorAction SilentlyContinue "
        "}"
    )
    print("COMMAND=powershell.exe -NoProfile -ExecutionPolicy Bypass -Command <redacted body>")
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass", "-Command", script],
        check=False,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=60.0,
    )
    if result.returncode != 0:
        raise RuntimeError(f"ESP32-S3 baseline Windows D4 failed with exit code {result.returncode}")


def reattach_to_wsl(host: str, bus_id: str, timeout_seconds: float) -> str:
    windows_result = subprocess.run(
        [
            "powershell.exe",
            "-NoProfile",
            "-Command",
            f"usbipd wsl attach --busid {bus_id} --distribution Ubuntu-22.04",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if windows_result.returncode != 0:
        result = subprocess.run(
        [
            "sudo",
            "-n",
            "/usr/bin/usbip",
            "attach",
            f"--remote={host}",
            f"--busid={bus_id}",
        ],
        check=False,
        capture_output=True,
        text=True,
        )
        if result.returncode != 0 and "Device busy (exported)" not in (
            result.stderr + result.stdout
        ):
            detail = (
                windows_result.stderr
                or windows_result.stdout
                or result.stderr
                or result.stdout
            ).strip()
            raise RuntimeError("cannot reattach ESP32-S3 baseline to WSL: " + detail)

    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        try:
            port = resolve_baseline_esp32_port("auto")
            require_baseline_port(port)
            return port
        except RuntimeError:
            time.sleep(0.25)
    raise RuntimeError("ESP32-S3 baseline did not reattach to WSL")


def capture_scan(port: str, timeout_seconds: float, maximum_bytes: int) -> None:
    with serial.Serial(
        port=None,
        baudrate=115200,
        timeout=0.1,
        write_timeout=1.0,
    ) as bridge:
        bridge.dtr = True
        bridge.rts = False
        bridge.port = port
        bridge.open()
        bridge.reset_input_buffer()
        runtime = capture_runtime(bridge, timeout_seconds, maximum_bytes)

    if FAIL_PATTERN.search(runtime):
        raise RuntimeError("ESP32-S3 baseline scan Sketch reported failure")
    firmware = FIRMWARE_PATTERN.search(runtime)
    count = COUNT_PATTERN.search(runtime)
    elapsed = ELAPSED_PATTERN.search(runtime)
    if firmware is None or count is None or elapsed is None or PASS_MARKER not in runtime:
        raise RuntimeError("ESP32-S3 baseline scan markers were incomplete after D4")
    if int(count.group(1)) <= 0:
        raise RuntimeError("ESP32-S3 baseline scan returned no networks after D4")
    print(f"ESP32_RA4_SWD_D4_FIRMWARE={firmware.group(1).strip()}")
    print(f"ESP32_RA4_SWD_D4_SCAN_COUNT={count.group(1)}")
    print(f"ESP32_RA4_SWD_D4_SCAN_ELAPSED_MS={elapsed.group(1)}")
    print("ESP32_RA4_SWD_D4_SCAN_PASS")


def main() -> int:
    args = parse_arguments()
    if args.capture_timeout_ms <= 0 or args.maximum_capture_bytes <= 0:
        raise RuntimeError("capture limits must be positive")

    bridge_port = resolve_baseline_esp32_port(args.bridge_port)
    require_baseline_port(bridge_port)
    host = resolve_wsl_usbip_host(args.wsl_usbip_host)
    bus_id = discover_windows_bus_id()
    print(f"Bridge: {bridge_port} (original ESP32-S3 {BASELINE_SERIAL})")
    print("Target: RA4M1 SWD/debug state only; no BL616CL, ESP32-S3, or RA4M1 Flash write.")

    try:
        run_windows_d4(bus_id)
    finally:
        bridge_port = reattach_to_wsl(host, bus_id, 10.0)

    capture_scan(
        bridge_port,
        args.capture_timeout_ms / 1000.0,
        args.maximum_capture_bytes,
    )
    print("ESP32_RA4_SWD_D4_BASELINE_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
