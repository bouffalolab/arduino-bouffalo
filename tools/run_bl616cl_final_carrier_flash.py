#!/usr/bin/env python3
"""Flash and boot the BL616CL final carrier through its FTDI control lines.

The final carrier exposes BOOT and RESET through one FTDI interface:
``DTR# -> BOOT`` and ``RTS# -> RESET``. WSL keeps that FTDI port open while
Windows FlashCube owns only the temporary BL616CL BootROM USB serial device.
This prevents modem-control state from changing between the reset pulse and
the FlashCube transaction.
"""

from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

import serial


ROOT_DIR = Path(__file__).resolve().parents[1]
DEFAULT_BUILD_DIR = ROOT_DIR / "build" / "bl616cl-bridge"
DEFAULT_WINDOWS_RUNNER = (
    ROOT_DIR / "tools" / "run_bl616cl_final_carrier_flash_windows.ps1"
)
DEFAULT_WINDOWS_DISTRIBUTION = "Ubuntu-22.04"


def require_file(path: Path) -> Path:
    resolved = path.resolve()
    if not resolved.is_file():
        raise RuntimeError(f"required file does not exist: {resolved}")
    return resolved


def tty_properties(port: str) -> dict[str, str]:
    device = Path(port)
    if not device.exists():
        raise RuntimeError(f"serial node does not exist: {port}")
    result = subprocess.run(
        [
            "udevadm",
            "info",
            "--query=property",
            f"--path=/sys/class/tty/{device.name}",
        ],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"cannot inspect {port}: {result.stderr.strip()}")
    return {
        key: value
        for line in result.stdout.splitlines()
        if "=" in line
        for key, value in [line.split("=", 1)]
    }


def resolve_ftdi_port(requested: str) -> str:
    if requested != "auto":
        candidates = [requested]
    else:
        candidates = [
            f"/dev/{path.name}"
            for path in sorted(Path("/sys/class/tty").glob("ttyUSB*"))
            if Path(f"/dev/{path.name}").exists()
        ]

    matches: list[str] = []
    for port in candidates:
        try:
            properties = tty_properties(port)
        except RuntimeError:
            continue
        if (
            properties.get("ID_VENDOR_ID", "").lower() == "0403"
            and properties.get("ID_MODEL_ID", "").lower() == "6001"
            and properties.get("ID_USB_INTERFACE_NUM", "").lower() == "00"
        ):
            matches.append(port)

    if len(matches) != 1:
        found = ", ".join(matches) if matches else "none"
        raise RuntimeError(
            "expected exactly one final-carrier FTDI "
            "(0403:6001 interface 00), found: " + found
        )
    return matches[0]


def wsl_unc_path(path: Path, distribution: str) -> str:
    return rf"\\wsl.localhost\{distribution}{path}"


def run_windows_flash(
    runner: Path,
    boot2: Path,
    partition: Path,
    application: Path,
    bootrom_port: str,
    distribution: str,
    baudrate: int,
    bootrom_timeout_seconds: int,
    bootrom_settle_ms: int,
    flash_timeout_seconds: int,
    application_chunk_bytes: int,
) -> None:
    command = [
        "powershell.exe",
        "-NoProfile",
        "-ExecutionPolicy",
        "Bypass",
        "-File",
        wsl_unc_path(runner, distribution),
        "-Boot2",
        wsl_unc_path(boot2, distribution),
        "-Partition",
        wsl_unc_path(partition, distribution),
        "-Application",
        wsl_unc_path(application, distribution),
        "-BootRomPort",
        bootrom_port,
        "-BaudRate",
        str(baudrate),
        "-BootRomTimeoutSeconds",
        str(bootrom_timeout_seconds),
        "-BootRomSettleMilliseconds",
        str(bootrom_settle_ms),
        "-FlashTimeoutSeconds",
        str(flash_timeout_seconds),
        "-ApplicationChunkBytes",
        str(application_chunk_bytes),
    ]
    print("COMMAND=" + " ".join(command))
    result = subprocess.run(command, check=False)
    if result.returncode != 0:
        raise RuntimeError("Windows FlashCube runner failed")


def wait_for_windows_bootrom_port(timeout_seconds: int) -> str:
    script = (
        "$deadline = (Get-Date).AddSeconds(" + str(timeout_seconds) + "); "
        "do { "
        "$matches = @(Get-PnpDevice -PresentOnly | "
        "Where-Object { $_.Status -eq 'OK' -and "
        "$_.InstanceId -match 'VID_FFFF&PID_FFFF' } | "
        "ForEach-Object { "
        "if ($_.FriendlyName -match '\\((COM\\d+)\\)') { $Matches[1] } "
        "}); "
        "if ($matches.Count -eq 1) { Write-Output $matches[0]; exit 0 }; "
        "if ($matches.Count -gt 1) { "
        "Write-Error ('expected exactly one ffff:ffff BL616CL BootROM port, found ' + "
        "$matches.Count); exit 2 }; "
        "Start-Sleep -Milliseconds 250 "
        "} while ((Get-Date) -lt $deadline); "
        "exit 1"
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", script],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode == 0:
        port = result.stdout.strip()
        if port:
            return port
    if result.returncode == 2:
        detail = result.stderr.strip() or result.stdout.strip()
        raise RuntimeError(detail or "ambiguous BL616CL BootROM serial ports")
    return ""


def wait_for_windows_normal_usb(timeout_seconds: int) -> None:
    script = (
        "$deadline = (Get-Date).AddSeconds(" + str(timeout_seconds) + "); "
        "do { "
        "$devices = @(Get-PnpDevice -PresentOnly | "
        "Where-Object { $_.InstanceId -match "
        "'VID_2341&PID_1002\\\\BL616CL-PROVISIONAL$' }); "
        "if ($devices.Count -eq 1) { "
        "$devices | Select-Object Status,Class,FriendlyName,InstanceId | Format-Table -AutoSize; "
        "exit 0 "
        "}; "
        "Start-Sleep -Milliseconds 250 "
        "} while ((Get-Date) -lt $deadline); "
        "throw 'BL616CL normal USB CDC/HID did not enumerate'"
    )
    command = ["powershell.exe", "-NoProfile", "-Command", script]
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("final-carrier normal USB enumeration failed")


def reset_into_bootrom(
    link: serial.Serial,
    boot_setup_seconds: float,
    reset_settle_seconds: float,
) -> None:
    # FTDI line names are active-low at the target: DTR# high releases BOOT,
    # while RTS# low asserts RESET.
    link.dtr = False
    time.sleep(boot_setup_seconds)
    link.rts = True
    time.sleep(reset_settle_seconds)
    link.rts = False


def reset_into_flash(link: serial.Serial, settle_seconds: float) -> None:
    link.dtr = True
    link.rts = True
    time.sleep(settle_seconds)
    link.rts = False


def capture_console(link: serial.Serial, duration_seconds: float) -> int:
    deadline = time.monotonic() + duration_seconds
    captured = bytearray()
    while time.monotonic() < deadline:
        payload = link.read(1024)
        if payload:
            captured.extend(payload)
    if captured:
        text = captured.decode("utf-8", errors="replace")
        redacted_lines = []
        for line in text.splitlines(keepends=True):
            if line.lstrip().startswith(("Exec ssid ", "Exec psk ")):
                newline = "\r\n" if line.endswith("\r\n") else "\n"
                redacted_lines.append("Exec credential <redacted>" + newline)
            else:
                redacted_lines.append(line)
        print("FINAL_CARRIER_CONSOLE_BEGIN")
        redacted = "".join(redacted_lines)
        print(redacted, end="" if redacted.endswith("\n") else "\n")
        print("FINAL_CARRIER_CONSOLE_END")
    print(f"FINAL_CARRIER_CONSOLE_BYTES={len(captured)}")
    return len(captured)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ftdi-port", default="auto")
    parser.add_argument("--wsl-distribution", default=DEFAULT_WINDOWS_DISTRIBUTION)
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
    parser.add_argument("--boot-setup-ms", type=int, default=100)
    parser.add_argument("--reset-settle-ms", type=int, default=150)
    parser.add_argument("--bootrom-timeout-seconds", type=int, default=15)
    parser.add_argument("--bootrom-settle-ms", type=int, default=1000)
    parser.add_argument("--bootrom-reset-attempts", type=int, default=3)
    parser.add_argument("--bootrom-retry-delay-ms", type=int, default=500)
    parser.add_argument("--flash-timeout-seconds", type=int, default=45)
    parser.add_argument(
        "--application-chunk-bytes",
        type=int,
        default=0,
        help=(
            "write the BL616CL application as independently verified chunks; "
            "zero writes the application in one FlashCube transaction"
        ),
    )
    parser.add_argument("--normal-usb-timeout-seconds", type=int, default=20)
    parser.add_argument("--console-capture-seconds", type=float, default=4.0)
    parser.add_argument(
        "--skip-flash",
        action="store_true",
        help="reset into the existing Flash application without writing BL616CL Flash",
    )
    parser.add_argument(
        "--bootrom-already-active",
        action="store_true",
        help=(
            "reuse an already enumerated ffff:ffff BootROM session instead of "
            "resetting into BootROM before FlashCube"
        ),
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.baudrate <= 0
        or args.boot_setup_ms < 0
        or args.reset_settle_ms <= 0
        or args.bootrom_timeout_seconds <= 0
        or args.bootrom_settle_ms < 0
        or args.bootrom_reset_attempts <= 0
        or args.bootrom_retry_delay_ms < 0
        or args.flash_timeout_seconds <= 0
        or args.application_chunk_bytes < 0
        or args.normal_usb_timeout_seconds <= 0
        or args.console_capture_seconds < 0
    ):
        raise RuntimeError("invalid timeout or baudrate")

    args.ftdi_port = resolve_ftdi_port(args.ftdi_port)
    runner = require_file(args.windows_runner)
    boot2 = require_file(args.boot2)
    partition = require_file(args.partition)
    application = require_file(args.application)
    properties = tty_properties(args.ftdi_port)
    print(
        f"FTDI: {args.ftdi_port} "
        f"({properties.get('ID_VENDOR_ID')}:{properties.get('ID_MODEL_ID')} "
        f"interface {properties.get('ID_USB_INTERFACE_NUM')})"
    )
    if args.skip_flash:
        print("Target: BL616CL Flash application boot only; Flash is untouched.")
    else:
        print("Target: BL616CL boot2, partition, and application Flash.")
    print("Target: RA4M1 Flash is untouched.")

    link = serial.Serial(port=None, baudrate=args.baudrate, timeout=0.1, write_timeout=0.1)
    # Opening the FTDI applies these modem-control values. Preserve BOOT high
    # when a caller explicitly reuses an existing BootROM session.
    link.dtr = False if args.bootrom_already_active else True
    link.rts = False
    link.port = args.ftdi_port
    link.open()
    flash_booted = False
    try:
        if not args.skip_flash:
            bootrom_port = ""
            if args.bootrom_already_active:
                bootrom_port = wait_for_windows_bootrom_port(
                    args.bootrom_timeout_seconds
                )
                print(f"FINAL_CARRIER_BOOTROM_REUSED port={bootrom_port}")
            else:
                for attempt in range(1, args.bootrom_reset_attempts + 1):
                    link.reset_input_buffer()
                    reset_into_bootrom(
                        link,
                        args.boot_setup_ms / 1000.0,
                        args.reset_settle_ms / 1000.0,
                    )
                    print(
                        "FINAL_CARRIER_BOOTROM_REQUESTED "
                        f"attempt={attempt}/{args.bootrom_reset_attempts}"
                    )
                    bootrom_port = wait_for_windows_bootrom_port(
                        args.bootrom_timeout_seconds
                    )
                    if bootrom_port:
                        print(
                            "FINAL_CARRIER_BOOTROM_ENUMERATED "
                            f"port={bootrom_port} attempt={attempt}"
                        )
                        break
                    if attempt < args.bootrom_reset_attempts:
                        time.sleep(args.bootrom_retry_delay_ms / 1000.0)
            if not bootrom_port:
                raise RuntimeError(
                    "BL616CL BootROM did not enumerate after "
                    f"{args.bootrom_reset_attempts} reset attempt(s)"
                )
            run_windows_flash(
                runner,
                boot2,
                partition,
                application,
                bootrom_port,
                args.wsl_distribution,
                args.baudrate,
                args.bootrom_timeout_seconds,
                args.bootrom_settle_ms,
                args.flash_timeout_seconds,
                args.application_chunk_bytes,
            )

        link.reset_input_buffer()
        reset_into_flash(link, args.reset_settle_ms / 1000.0)
        print("FINAL_CARRIER_FLASH_BOOT_REQUESTED")
        capture_console(link, args.console_capture_seconds)
        wait_for_windows_normal_usb(args.normal_usb_timeout_seconds)
        flash_booted = True
        print("FINAL_CARRIER_FLASH_AND_BOOT_PASS")
    finally:
        # A BOOT strap change takes effect only on reset. Recover failed
        # FlashCube attempts back to Flash boot before releasing the FTDI so
        # a failed ISP transaction never leaves the carrier in BootROM.
        if not flash_booted:
            try:
                reset_into_flash(link, args.reset_settle_ms / 1000.0)
            except serial.SerialException:
                pass
        link.close()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
