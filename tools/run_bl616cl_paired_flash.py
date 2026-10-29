#!/usr/bin/env python3
"""Flash the paired BL616CL while relay control remains in WSL.

Windows owns CKLink/FlashCube. WSL owns the FTDI relay, so this coordinator
keeps the relay on the CKLink download path for FlashCube and switches it to
the RA4M1 runtime path only for the CKLink reset that starts the application.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
import run_ra4_at_uart_gate as paired
from uno_r4_usb_identity import resolve_paired_bl616cl_port, resolve_relay_port


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_BUILD_DIR = ROOT_DIR / "build" / "bl616cl-bridge"
DEFAULT_WINDOWS_RUNNER = ROOT_DIR / "tools" / "run_bl616cl_paired_windows.ps1"


def run_windows(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("Windows paired BL616CL flash helper failed")


def require_file(path: Path) -> Path:
    resolved = path.resolve()
    if not resolved.is_file():
        raise RuntimeError(f"required image does not exist: {resolved}")
    return resolved


def wsl_unc_path(path: Path) -> str:
    return "\\\\wsl.localhost\\Ubuntu-22.04" + str(path)


def wait_for_bridge(timeout_seconds: float) -> str:
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        try:
            port = resolve_paired_bl616cl_port("auto")
            paired.require_port_identity(port, "2341", "1002", "00")
            return port
        except RuntimeError:
            time.sleep(0.25)
    raise RuntimeError("paired BL616CL bridge did not re-enumerate in WSL")


def attach_bridge_to_wsl(
    distribution: str,
    attempts: int,
    retry_delay_seconds: float,
) -> None:
    check_command = (
        "$devices = @((usbipd state | ConvertFrom-Json).Devices | "
        "Where-Object { "
        "$_.BusId -ne $null -and "
        "$_.InstanceId.EndsWith('\\BL616CL-PROVISIONAL', "
        "[System.StringComparison]::OrdinalIgnoreCase) "
        "}); "
        "if ($devices.Count -ne 1) { "
        "throw \"expected exactly one paired BL616CL USB device, found $($devices.Count)\" "
        "}; "
        "$busid = $devices[0].BusId; "
        f"usbipd wsl attach --busid $busid --distribution {distribution}"
    )
    for attempt in range(1, attempts + 1):
        command = ["powershell.exe", "-NoProfile", "-Command", check_command]
        print("COMMAND=" + " ".join(command))
        if subprocess.run(command, check=False).returncode == 0:
            return
        if attempt < attempts:
            print(
                "WSL_USBIP_ATTACH_RETRY="
                f"{attempt}/{attempts} delay_ms={int(retry_delay_seconds * 1000)}"
            )
            time.sleep(retry_delay_seconds)
    raise RuntimeError("could not reattach paired BL616CL to WSL after reset")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--bridge-port",
        default="auto",
        help=(
            "paired BL616CL CDC path before reset, or auto to resolve it by "
            "USB serial identity"
        ),
    )
    parser.add_argument("--relay-port", default="/dev/ttyUSB0")
    parser.add_argument("--cklink-port", default="COM30")
    parser.add_argument("--wsl-distribution", default="Ubuntu-22.04")
    parser.add_argument("--windows-runner", type=Path, default=DEFAULT_WINDOWS_RUNNER)
    parser.add_argument(
        "--boot2",
        type=Path,
        default=DEFAULT_BUILD_DIR / "UNOR4BL616CLBridge.ino.boot2.bin",
    )
    parser.add_argument(
        "--partition",
        type=Path,
        default=DEFAULT_BUILD_DIR / "UNOR4BL616CLBridge.ino.partition.bin",
    )
    parser.add_argument(
        "--application",
        type=Path,
        default=DEFAULT_BUILD_DIR / "UNOR4BL616CLBridge.ino.bin",
    )
    parser.add_argument("--baudrate", type=int, default=2000000)
    parser.add_argument("--boot-timeout-ms", type=int, default=20000)
    parser.add_argument("--attach-attempts", type=int, default=3)
    parser.add_argument("--attach-retry-ms", type=int, default=1000)
    parser.add_argument(
        "--skip-flash",
        action="store_true",
        help=(
            "Reset and boot the paired BL616CL without writing boot2, "
            "partition, or application Flash."
        ),
    )
    parser.add_argument(
        "--allow-missing-bridge",
        action="store_true",
        help=(
            "Permit recovery flashing when the paired BL616CL USB device has "
            "failed enumeration; the relay and CKLink identities remain required."
        ),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.baudrate <= 0
        or args.boot_timeout_ms <= 0
        or args.attach_attempts <= 0
        or args.attach_retry_ms < 0
    ):
        raise RuntimeError("invalid timeout or baudrate")

    boot2 = require_file(args.boot2)
    partition = require_file(args.partition)
    application = require_file(args.application)
    runner = require_file(args.windows_runner)
    args.relay_port = resolve_relay_port(args.relay_port)
    if not args.allow_missing_bridge:
        args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
        paired.require_port_identity(args.bridge_port, "2341", "1002", "00")
    paired.require_port_identity(args.relay_port, "0403", "6010", "00")
    if args.allow_missing_bridge:
        print(
            "Bridge:         recovery mode; paired BL616CL USB identity will be "
            "verified after flash/reset"
        )
    else:
        print(f"Bridge:         {args.bridge_port} (2341:1002 paired BL616CL)")
    print(f"Relay:          {args.relay_port} (0403:6010 interface 00)")
    print(f"CKLink:         {args.cklink_port} (validated by Windows helper)")
    if args.skip_flash:
        print("Target: paired BL616CL reset and boot only; Flash is untouched.")
    else:
        print("Target: paired BL616CL boot2, partition, and application Flash.")
    print("Target: RA4M1 and original ESP32-S3 Flash are untouched.")

    common = [
        "powershell.exe",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        wsl_unc_path(runner),
        "-Boot2",
        wsl_unc_path(boot2),
        "-Partition",
        wsl_unc_path(partition),
        "-Application",
        wsl_unc_path(application),
        "-CkLinkPort",
        args.cklink_port,
        "-BaudRate",
        str(args.baudrate),
        "-SkipRelay",
    ]

    relay = serial.Serial(
        args.relay_port,
        baudrate=115200,
        timeout=0.2,
        write_timeout=0.2,
    )
    relay.dtr = False
    bridge_booted = False
    try:
        if args.skip_flash:
            relay.rts = False
            time.sleep(0.05)
            print("Relay route: RA4M1 runtime (RTS=False, physical RTS# HIGH).")
            run_windows(common + ["-SkipFlash"])
        else:
            relay.rts = True
            print("Relay route: CKLink download (RTS=True, physical RTS# LOW).")
            run_windows(common + ["-SkipPostFlashBoot"])

            relay.rts = False
            time.sleep(0.05)
            print("Relay route: RA4M1 runtime (RTS=False, physical RTS# HIGH).")
            run_windows(common + ["-SkipFlash"])
        attach_bridge_to_wsl(
            args.wsl_distribution,
            args.attach_attempts,
            args.attach_retry_ms / 1000.0,
        )
        args.bridge_port = wait_for_bridge(args.boot_timeout_ms / 1000.0)
        print(f"Bridge:         {args.bridge_port} (2341:1002 paired BL616CL)")
        bridge_booted = True
        print("PAIRED_BL616CL_FLASH_AND_BOOT_PASS")
    finally:
        if relay.is_open:
            relay.rts = not bridge_booted
            relay.close()
            if bridge_booted:
                print("Relay route: RA4M1 runtime retained (RTS=False, physical RTS# HIGH).")
            else:
                print("Relay route: CKLink download restored (RTS=True, physical RTS# LOW).")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
