#!/usr/bin/env python3
"""Validate a Windows pyOCD CMSIS-DAP session on the paired BL616CL board."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path

from read_bl616cl_windows_telemetry import (
    discover_windows_bus_id,
    reattach_bridge,
    resolve_wsl_usbip_host,
)
from uno_r4_usb_identity import resolve_paired_bl616cl_port, tty_properties


ROOT_DIR = Path(__file__).resolve().parents[1]
CPUID = "410fc241"


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--wsl-distribution", default="Ubuntu-22.04")
    parser.add_argument("--wsl-usbip-host", default="auto")
    parser.add_argument("--frequency-hz", type=int, default=100000)
    parser.add_argument("--capture-timeout-ms", type=int, default=45000)
    parser.add_argument("--skip-scan-recovery", action="store_true")
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


def require_pyocd_result(output: str) -> None:
    expected = (
        "Arduino UNO WiFi R4 CMSIS-DAP",
        "BL616CL-PROVISIONAL",
        "Setting SWD clock to 100 kHz",
        "Successfully halted device",
        f"e000ed00:  {CPUID}",
        "00000000:",
        "20000000:",
        "Successfully resumed device",
        "Resetting target",
        "Core 0 (None):  Running",
    )
    missing = [marker for marker in expected if marker.lower() not in output.lower()]
    if missing:
        raise RuntimeError(
            "pyOCD D5 output did not contain required markers: " + ", ".join(missing)
        )


def run_windows_pyocd(bus_id: str, frequency_hz: int) -> str:
    # Windows owns the HID device during this short session. The registered
    # ESP32-S3 comparison board remains attached to WSL, so the pyOCD UID is
    # both required and sufficient to select only the BL616CL carrier.
    script = (
        "$ErrorActionPreference = 'Stop'; "
        f"usbipd wsl detach --busid {bus_id}; "
        "Start-Sleep -Milliseconds 1000; "
        "python -m pyocd list --probes --verbose; "
        "if ($LASTEXITCODE -ne 0) { "
        "throw \"Windows pyOCD probe listing failed with exit code $LASTEXITCODE\" "
        "} "
        "python -m pyocd commander --target cortex_m --uid BL616CL-PROVISIONAL "
        f"--frequency {frequency_hz} --no-wait "
        "-c halt "
        "-c \"read32 0xE000ED00 4\" "
        "-c \"read32 0x00000000 16\" "
        "-c \"read32 0x20000000 16\" "
        "-c continue "
        "-c reset "
        "-c status; "
        "if ($LASTEXITCODE -ne 0) { "
        "throw \"Windows pyOCD commander failed with exit code $LASTEXITCODE\" "
        "}"
    )
    command = ["powershell.exe", "-NoProfile", "-Command", script]
    print("COMMAND=" + " ".join(command[:4]) + " <redacted PowerShell body>")
    result = subprocess.run(
        command,
        check=False,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        timeout=90.0,
    )
    output = result.stdout + result.stderr
    if result.stdout:
        print(result.stdout, end="")
    if result.stderr:
        print(result.stderr, end="", file=sys.stderr)
    if result.returncode != 0:
        raise RuntimeError(f"Windows pyOCD D5 session failed with exit code {result.returncode}")
    return output


def main() -> int:
    args = parse_arguments()
    if args.frequency_hz <= 0 or args.capture_timeout_ms <= 0:
        raise RuntimeError("frequency and capture timeout must be positive")

    bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier(bridge_port)
    bus_id = discover_windows_bus_id()
    host = resolve_wsl_usbip_host(args.wsl_usbip_host)
    print(f"BL616CL_FINAL_CARRIER_PORT={bridge_port}")
    print(f"BL616CL_WINDOWS_BUS_ID={bus_id}")
    print("Target: RA4M1 SWD halt/read/resume/reset-run only; no Flash is written.")

    output = ""
    try:
        output = run_windows_pyocd(bus_id, args.frequency_hz)
    finally:
        reattach_bridge(
            args.wsl_distribution,
            host,
            bus_id,
            bridge_port,
            15.0,
        )
    require_final_carrier(bridge_port)
    require_pyocd_result(output)
    print("BL616CL_RA4_SWD_D5_PYOCD_SESSION_PASS")

    if not args.skip_scan_recovery:
        recovery = [
            sys.executable,
            str(ROOT_DIR / "tools" / "run_bl616cl_swd_d4_scan_recovery.py"),
            "--bridge-port",
            bridge_port,
            "--capture-timeout-ms",
            str(args.capture_timeout_ms),
        ]
        print("COMMAND=" + " ".join(recovery))
        if subprocess.run(recovery, check=False).returncode != 0:
            raise RuntimeError("D4 recovery/scan validation after pyOCD failed")
        require_final_carrier(bridge_port)
        print("BL616CL_RA4_SWD_D5_PYOCD_SCAN_RECOVERY_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
