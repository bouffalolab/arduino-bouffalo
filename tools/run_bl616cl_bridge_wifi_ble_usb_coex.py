#!/usr/bin/env python3
"""Flash and validate standalone BL616CL Wi-Fi + BLE + USB coexistence."""

from __future__ import annotations

import argparse
import os
from pathlib import Path
import subprocess
import sys
import time

try:
    import serial
except ImportError as exc:
    print(f"pyserial is required: {exc}", file=sys.stderr)
    raise SystemExit(2)

sys.path.insert(0, str(Path(__file__).resolve().parent))
from verify_bl616cl_usb_composite import (  # noqa: E402
    require_cdc_node,
    require_hidraw_node,
    require_interface_classes,
    verify_cdc_loopback,
    verify_hid,
    wait_for_device,
)


EXPECTED_PROPERTIES = {
    "ID_VENDOR_ID": "42bf",
    "ID_MODEL_ID": "b210",
}
EXPECTED_SERIAL_SHORTS = frozenset(
    {
        "FactoryAIOT_Prog_B2DDDD",
        "FactoryAIOT_Prog_B2E0C2",
    }
)
RESET_COMMANDS = (b"BOUFFALOLAB5555RTS0", b"BOUFFALOLAB5555RTS1")
RESET_ATTEMPTS = 3
RESET_RETRY_DELAY_SECONDS = 2.0
PREFLASH_RESET_SETTLE_SECONDS = 2.0
USBIPD_ATTACH_TIMEOUT_SECONDS = 30.0
SUCCESS_PREFIX = b"BL616CL_SMOKE:USB_COEX_STABLE_DONE cycles=3"
FAILURE_PREFIX = b"BL616CL_SMOKE:FAIL "
USB_ECHO_PREFIX = b"BL616CL_SMOKE:USB_COEX_HOST_ECHO_REQUIRED cycle="


def port_properties(port: str) -> dict[str, str]:
    result = subprocess.run(
        ["udevadm", "info", "--query=property", f"--name={port}"],
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    properties: dict[str, str] = {}
    for line in result.stdout.splitlines():
        key, separator, value = line.partition("=")
        if separator:
            properties[key] = value
    return properties


def verify_target(port: str) -> str:
    if not Path(port).is_char_device():
        raise RuntimeError(f"missing serial port: {port}")

    properties = port_properties(port)
    mismatches = [
        f"{key}={properties.get(key, '<missing>')}"
        for key, expected in EXPECTED_PROPERTIES.items()
        if properties.get(key) != expected
    ]
    serial_number = properties.get("ID_SERIAL_SHORT")
    if serial_number not in EXPECTED_SERIAL_SHORTS:
        mismatches.append(
            f"ID_SERIAL_SHORT={serial_number or '<missing>'} "
            f"(expected one of {', '.join(sorted(EXPECTED_SERIAL_SHORTS))})"
        )
    if mismatches:
        raise RuntimeError(
            f"refusing to use {port}; expected dedicated BL616CL CKLink-Lite "
            f"identity, observed {', '.join(mismatches)}"
        )
    return serial_number


def flash(
    flash_tool: Path,
    boot2: Path,
    partition: Path,
    firmware: Path,
    port: str,
) -> None:
    subprocess.run(
        [
            str(flash_tool),
            "--interface=uart",
            "--chipname=bl616cl",
            f"--port={port}",
            "--baudrate=2000000",
            "write_flash_files",
            "0x0",
            str(boot2),
            "0xE000",
            str(partition),
            "0x10000",
            str(firmware),
        ],
        check=True,
    )


def host_usb_echo(cdc_port: str, hidraw: str, cycle: int) -> None:
    started_at = time.monotonic()
    verify_cdc_loopback(cdc_port)
    cdc_elapsed_ms = (time.monotonic() - started_at) * 1000.0
    verify_hid(hidraw, require_feature_io=True)
    hid_elapsed_ms = ((time.monotonic() - started_at) * 1000.0) - cdc_elapsed_ms
    print(
        f"USB CDC + HID echo: PASS cycle={cycle} "
        f"cdc_ms={cdc_elapsed_ms:.1f} hid_ms={hid_elapsed_ms:.1f}"
    )


def reset_link(link: serial.Serial) -> None:
    link.dtr = False
    link.rts = False
    link.reset_input_buffer()
    for command in RESET_COMMANDS:
        link.write(command)
        link.flush()
        time.sleep(0.05)


def reset_target(port: str) -> serial.Serial:
    last_error: serial.SerialTimeoutException | None = None
    for attempt in range(RESET_ATTEMPTS):
        link: serial.Serial | None = None
        try:
            link = serial.Serial(port, 2000000, timeout=0.05, write_timeout=2)
            reset_link(link)
            return link
        except serial.SerialTimeoutException as error:
            last_error = error
            if link is not None:
                link.close()
            if attempt + 1 == RESET_ATTEMPTS:
                break
            print(
                f"CKLink reset write timed out; retrying "
                f"({attempt + 2}/{RESET_ATTEMPTS})"
            )
            time.sleep(RESET_RETRY_DELAY_SECONDS)
    assert last_error is not None
    raise last_error


def ensure_hidraw_access(hidraw: str, prepare_via_wsl_root: bool, distribution: str) -> None:
    if os.access(hidraw, os.R_OK | os.W_OK):
        return
    if not prepare_via_wsl_root:
        raise RuntimeError(
            f"HID node is not readable and writable: {hidraw}; grant access or rerun "
            "with --prepare-hidraw-via-wsl-root in this WSL test environment"
        )

    subprocess.run(
        [
            "powershell.exe",
            "-NoProfile",
            "-Command",
            (
                f"wsl.exe -d {distribution} -u root -- "
                f"chmod a+rw {hidraw}"
            ),
        ],
        check=True,
    )
    if not os.access(hidraw, os.R_OK | os.W_OK):
        raise RuntimeError(f"failed to grant temporary read/write access to {hidraw}")


def reattach_native_usb_via_usbipd(distribution: str) -> None:
    deadline = time.monotonic() + USBIPD_ATTACH_TIMEOUT_SECONDS
    command = (
        "$ErrorActionPreference = 'Stop'; "
        "$matches = @(usbipd list | Select-String -Pattern "
        "'^\\s*(?<busid>\\S+)\\s+2341:1002\\s+'); "
        "if ($matches.Count -ne 1) { "
        "throw \"expected exactly one BL616CL native USB device, found $($matches.Count)\" "
        "}; "
        "$busid = $matches[0].Matches[0].Groups['busid'].Value; "
        f"usbipd wsl attach --busid $busid --distribution {distribution}"
    )

    while time.monotonic() < deadline:
        result = subprocess.run(
            ["powershell.exe", "-NoProfile", "-Command", command],
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
        )
        if result.returncode == 0:
            print("native USB: reattached to WSL via usbipd")
            return
        time.sleep(0.5)

    raise RuntimeError(
        "native BL616CL USB did not reattach to WSL after reset; "
        "usbipd could not find exactly one 2341:1002 device"
    )


def capture(
    link: serial.Serial,
    timeout_seconds: float,
    cdc_port: str,
    hidraw: str,
) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    captured = bytearray()
    pending = bytearray()
    echoed_cycles: set[int] = set()

    while time.monotonic() < deadline:
        waiting = link.in_waiting
        if waiting:
            chunk = link.read(waiting)
            captured.extend(chunk)
            pending.extend(chunk)
            while b"\n" in pending:
                line, _, remainder = pending.partition(b"\n")
                pending = bytearray(remainder)
                line = line.rstrip(b"\r")
                if line.startswith(USB_ECHO_PREFIX):
                    try:
                        cycle = int(line.rsplit(b"=", maxsplit=1)[1])
                    except (IndexError, ValueError) as error:
                        raise RuntimeError(
                            f"malformed USB echo marker: {line!r}"
                        ) from error
                    if cycle in echoed_cycles:
                        raise RuntimeError(f"duplicate USB echo marker for cycle {cycle}")
                    host_usb_echo(cdc_port, hidraw, cycle)
                    echoed_cycles.add(cycle)
            if SUCCESS_PREFIX in captured or FAILURE_PREFIX in captured:
                break
        else:
            time.sleep(0.01)

    if SUCCESS_PREFIX in captured and echoed_cycles != {1, 2, 3}:
        raise RuntimeError(
            "runtime completed without exactly one CDC/HID echo per cycle; "
            f"observed cycles: {sorted(echoed_cycles)}"
        )
    return bytes(captured)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyACM1")
    parser.add_argument("--boot2", required=True, type=Path)
    parser.add_argument("--partition", required=True, type=Path)
    parser.add_argument("--firmware", required=True, type=Path)
    parser.add_argument("--flash-tool", required=True, type=Path)
    parser.add_argument("--log", required=True, type=Path)
    parser.add_argument("--timeout", type=float, default=120.0)
    parser.add_argument(
        "--skip-flash",
        action="store_true",
        help="Validate the already-programmed image after resetting the target.",
    )
    parser.add_argument("--prepare-hidraw-via-wsl-root", action="store_true")
    parser.add_argument(
        "--reattach-native-usb-via-usbipd",
        action="store_true",
        help="Reattach the reset BL616CL native USB device to WSL before CDC/HID checks.",
    )
    parser.add_argument(
        "--wsl-distribution",
        default=os.environ.get("WSL_DISTRO_NAME", "Ubuntu-22.04"),
    )
    args = parser.parse_args()

    for name, path in (
        ("boot2", args.boot2),
        ("partition", args.partition),
        ("firmware", args.firmware),
    ):
        if not path.is_file():
            raise RuntimeError(f"{name} not found: {path}")
    if not args.flash_tool.is_file():
        raise RuntimeError(f"FlashCube tool not found: {args.flash_tool}")

    serial_number = verify_target(args.port)
    print(f"target: {args.port} BL616CL CKLink-Lite {serial_number}")
    if args.skip_flash:
        print("flash: skipped; validating the already-programmed image")
    else:
        # CKLink-Lite enters the BL616CL download/reset sequence through its
        # VCOM command pair. FlashCube must open the port after that sequence.
        preflash_link = reset_target(args.port)
        preflash_link.close()
        time.sleep(PREFLASH_RESET_SETTLE_SECONDS)
        print("pre-flash reset: requested")
        flash(args.flash_tool, args.boot2, args.partition, args.firmware, args.port)

    verify_target(args.port)
    link = reset_target(args.port)
    try:
        if args.reattach_native_usb_via_usbipd:
            reattach_native_usb_via_usbipd(args.wsl_distribution)
        device = wait_for_device(30.0)
        interfaces = require_interface_classes(device)
        cdc_port = require_cdc_node(interfaces)
        hidraw = require_hidraw_node(interfaces)
        ensure_hidraw_access(
            hidraw,
            args.prepare_hidraw_via_wsl_root,
            args.wsl_distribution,
        )
        print(f"USB device: {device.name}")
        print(f"CDC node: {cdc_port}")
        print(f"HID node: {hidraw}")

        captured = capture(link, args.timeout, cdc_port, hidraw)
    finally:
        link.close()

    args.log.parent.mkdir(parents=True, exist_ok=True)
    args.log.write_bytes(captured)

    safe_output = captured.decode("ascii", errors="replace")
    print(safe_output, end="" if safe_output.endswith("\n") else "\n")
    if SUCCESS_PREFIX in captured:
        print(f"PASS: Wi-Fi + BLE + USB coexistence verified; log: {args.log}")
        return 0
    if FAILURE_PREFIX in captured:
        print(f"FAIL: target reported smoke failure; log: {args.log}", file=sys.stderr)
        return 1

    print(
        f"FAIL: no runtime marker within {args.timeout:.0f}s; log: {args.log}",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
