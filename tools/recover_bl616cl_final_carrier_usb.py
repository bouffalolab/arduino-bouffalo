#!/usr/bin/env python3
"""Recover the final BL616CL carrier USB ownership without rewriting Flash."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

from read_bl616cl_windows_telemetry import (
    discover_wsl_usbip_bus_id,
    reattach_bridge,
    resolve_wsl_usbip_host,
)
from uno_r4_usb_identity import tty_properties


ROOT_DIR = Path(__file__).resolve().parents[1]
FLASH_BOOT = ROOT_DIR / "tools" / "run_bl616cl_final_carrier_flash.py"
EXPECTED_SERIAL = "BL616CL-PROVISIONAL"
WSL_DISTRIBUTION = "Ubuntu-22.04"


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="/dev/ttyACM0")
    parser.add_argument("--ftdi-port", default="/dev/ttyUSB0")
    parser.add_argument("--normal-usb-timeout-seconds", type=int, default=30)
    parser.add_argument("--reattach-timeout-seconds", type=int, default=15)
    return parser.parse_args()


def require_bridge_identity(port: str) -> None:
    properties = tty_properties(port)
    if (
        properties.get("ID_VENDOR_ID", "").lower() != "2341"
        or properties.get("ID_MODEL_ID", "").lower() != "1002"
        or properties.get("ID_USB_INTERFACE_NUM", "").lower() != "00"
        or properties.get("ID_SERIAL_SHORT") != EXPECTED_SERIAL
    ):
        raise RuntimeError(f"{port} is not the final-carrier BL616CL CDC interface")


def windows_bus_id() -> str:
    script = (
        "$ErrorActionPreference = 'Stop'; "
        "$devices = @((usbipd state | ConvertFrom-Json).Devices | "
        "Where-Object { $_.BusId -ne $null -and "
        "$_.InstanceId.EndsWith('\\BL616CL-PROVISIONAL', "
        "[System.StringComparison]::OrdinalIgnoreCase) }); "
        "if ($devices.Count -ne 1) { "
        "throw \"expected exactly one final-carrier BL616CL USB device, found $($devices.Count)\" "
        "}; "
        "Write-Output $devices[0].BusId"
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", script],
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=10.0,
    )
    if result.returncode != 0 or not result.stdout.strip():
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(
            "cannot identify the final-carrier BL616CL USB/IP bus ID" +
            (f": {detail}" if detail else "")
        )
    return result.stdout.strip()


def main() -> int:
    args = parse_arguments()
    if args.normal_usb_timeout_seconds <= 0 or args.reattach_timeout_seconds <= 0:
        raise RuntimeError("recovery timeouts must be positive")

    print("BL616CL_FINAL_CARRIER_USB_RECOVERY_STARTED")
    command = [
        sys.executable,
        str(FLASH_BOOT),
        "--ftdi-port",
        args.ftdi_port,
        "--skip-flash",
        "--console-capture-seconds",
        "0",
        "--normal-usb-timeout-seconds",
        str(args.normal_usb_timeout_seconds),
    ]
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("BL616CL Flash boot recovery failed")

    host = resolve_wsl_usbip_host("auto")
    try:
        bus_id = windows_bus_id()
    except RuntimeError:
        # A broken WSL-to-Windows interop channel must not prevent recovery:
        # the USB/IP service itself exposes the same stable PnP serial.
        bus_id = discover_wsl_usbip_bus_id(host)
        print("BL616CL_FINAL_CARRIER_USBIP_DISCOVERY=direct")
    reattach_bridge(
        WSL_DISTRIBUTION,
        host,
        bus_id,
        args.bridge_port,
        args.reattach_timeout_seconds,
    )
    print("BL616CL_FINAL_CARRIER_USB_RECOVERY_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
