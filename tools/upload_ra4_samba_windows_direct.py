#!/usr/bin/env python3
"""Write paired RA4M1 Flash through Windows COM34, then reattach WSL USB."""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_ra4_wifis3_scan_smoke import require_port_identity
from uno_r4_usb_identity import resolve_paired_bl616cl_port, resolve_relay_port


ROOT_DIR = Path(__file__).resolve().parents[1]
WINDOWS_UPLOADER = ROOT_DIR / "tools" / "upload_ra4_samba_split_windows.ps1"
DEFAULT_DISTRIBUTION = "Ubuntu-22.04"


def run_command(command: list[str], description: str) -> subprocess.CompletedProcess[str]:
    print("COMMAND=" + " ".join(command))
    result = subprocess.run(
        command,
        check=False,
        text=True,
        encoding="utf-8",
        errors="replace",
        capture_output=True,
    )
    if result.stdout:
        print(result.stdout, end="" if result.stdout.endswith("\n") else "\n")
    if result.stderr:
        print(result.stderr, end="" if result.stderr.endswith("\n") else "\n", file=sys.stderr)
    if result.returncode != 0:
        raise RuntimeError(f"{description} failed with exit status {result.returncode}")
    return result


def unc_path(path: Path) -> str:
    return "//wsl.localhost/Ubuntu-22.04" + str(path.resolve())


def paired_bus_id() -> str:
    command = (
        "$devices = @((usbipd state | ConvertFrom-Json).Devices | "
        "Where-Object { $_.BusId -ne $null -and "
        "$_.InstanceId.EndsWith('\\BL616CL-PROVISIONAL', "
        "[System.StringComparison]::OrdinalIgnoreCase) }); "
        "if ($devices.Count -ne 1) { "
        "throw \"expected exactly one paired BL616CL USB device, found $($devices.Count)\" "
        "}; "
        "Write-Output $devices[0].BusId"
    )
    result = run_command(
        ["powershell.exe", "-NoProfile", "-Command", command],
        "paired BL616CL USB bus lookup",
    )
    bus_ids = [line.strip() for line in result.stdout.splitlines() if line.strip()]
    if len(bus_ids) != 1:
        raise RuntimeError(f"expected one paired BL616CL USB bus ID, got: {bus_ids}")
    return bus_ids[0]


def wait_for_windows_bridge(
    windows_port: str,
    timeout_seconds: float,
) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        command = (
            "$devices = @(Get-PnpDevice -PresentOnly | "
            "Where-Object { "
            f"$_.FriendlyName -match '\\({windows_port}\\)' -and "
            "$_.InstanceId -match 'VID_2341&PID_1002' "
            "}); "
            "if ($devices.Count -eq 1) { exit 0 }; exit 1"
        )
        if subprocess.run(
            ["powershell.exe", "-NoProfile", "-Command", command],
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        ).returncode == 0:
            return
        time.sleep(0.25)
    raise RuntimeError(
        f"paired BL616CL Windows bridge did not enumerate at {windows_port}"
    )


def wait_for_paired_bus_id(timeout_seconds: float) -> str:
    deadline = time.monotonic() + timeout_seconds
    last_error: BaseException | None = None
    while time.monotonic() < deadline:
        try:
            return paired_bus_id()
        except RuntimeError as error:
            last_error = error
            time.sleep(0.25)
    assert last_error is not None
    raise last_error


def wait_for_wsl_bridge(port: str, timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        try:
            require_port_identity(port, "2341", "1002", "00")
            return
        except RuntimeError:
            time.sleep(0.25)
    raise RuntimeError(f"paired BL616CL bridge did not re-enumerate in WSL at {port}")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--windows-bridge-port", default="COM34")
    parser.add_argument("--wsl-distribution", default=DEFAULT_DISTRIBUTION)
    parser.add_argument("--runtime-settle-ms", type=int, default=500)
    parser.add_argument("--attach-timeout-ms", type=int, default=15000)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    image = args.image.resolve()
    if not image.is_file():
        raise RuntimeError(f"RA4M1 image does not exist: {image}")
    if args.runtime_settle_ms < 0 or args.attach_timeout_ms <= 0:
        raise RuntimeError("invalid timeout")

    require_port_identity(args.bridge_port, "2341", "1002", "00")
    require_port_identity(args.relay_port, "0403", "6010", "00")
    if not WINDOWS_UPLOADER.is_file():
        raise RuntimeError(f"Windows uploader does not exist: {WINDOWS_UPLOADER}")

    bus_id = paired_bus_id()
    print(f"Bridge: {args.bridge_port} (paired BL616CL 2341:1002 interface 00)")
    print(f"Relay:  {args.relay_port} (0403:6010 runtime route remains selected)")
    print(f"Windows bridge port: {args.windows_bridge_port}; USB bus: {bus_id}")
    print("Target: paired RA4M1 application Flash only; BL616CL and ESP32-S3 Flash untouched.")

    run_command(
        [
            "powershell.exe",
            "-NoProfile",
            "-Command",
            f"usbipd wsl detach --busid {bus_id}",
        ],
        "detach paired bridge from WSL",
    )
    try:
        wait_for_windows_bridge(args.windows_bridge_port, args.attach_timeout_ms / 1000.0)
        run_command(
            [
                "powershell.exe",
                "-NoProfile",
                "-ExecutionPolicy",
                "Bypass",
                "-File",
                unc_path(WINDOWS_UPLOADER),
                "-ImagePath",
                unc_path(image),
                "-BridgePort",
                args.windows_bridge_port,
                "-Direct",
                "-SkipRuntimeSmoke",
                "-RuntimeSettleMilliseconds",
                str(args.runtime_settle_ms),
            ],
            "Windows direct RA4M1 upload",
        )
    finally:
        current_bus_id = wait_for_paired_bus_id(args.attach_timeout_ms / 1000.0)
        run_command(
            [
                "powershell.exe",
                "-NoProfile",
                "-Command",
                f"usbipd wsl attach --busid {current_bus_id} --distribution {args.wsl_distribution}",
            ],
            "reattach paired bridge to WSL",
        )

    wait_for_wsl_bridge(args.bridge_port, args.attach_timeout_ms / 1000.0)
    print("RA4_WINDOWS_DIRECT_UPLOAD_AND_WSL_REATTACH_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
