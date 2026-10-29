#!/usr/bin/env python3
"""Restart the registered ESP32-S3 baseline board's RA4M1 without Flash writes."""

from __future__ import annotations

import argparse
import sys
import time

import serial

from uno_r4_usb_identity import BASELINE_ESP32_SERIALS, resolve_baseline_esp32_port, tty_properties
from upload_ra4_samba_direct import initialize_samba
from upload_ra4_samba_split import open_serial, write_all


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--settle-ms", type=int, default=500)
    return parser.parse_args()


def require_baseline_port(port: str) -> None:
    properties = tty_properties(port)
    if (
        properties.get("ID_VENDOR_ID", "").lower() != "2341"
        or properties.get("ID_MODEL_ID", "").lower() != "1002"
        or properties.get("ID_USB_INTERFACE_NUM", "").lower() != "01"
        or properties.get("ID_SERIAL_SHORT") not in BASELINE_ESP32_SERIALS
    ):
        raise RuntimeError(f"{port} is not a registered original ESP32-S3 baseline board")


def open_baseline_serial(port: str, baudrate: int, timeout: float) -> serial.Serial:
    link = serial.Serial(
        port=None,
        baudrate=baudrate,
        timeout=timeout,
        write_timeout=timeout,
    )
    link.dtr = True
    link.rts = False
    link.port = port
    link.open()
    return link


def main() -> int:
    args = parse_arguments()
    if args.settle_ms < 0:
        raise RuntimeError("settle time must not be negative")

    port = resolve_baseline_esp32_port(args.bridge_port)
    require_baseline_port(port)
    print(f"ESP32_BASELINE_PORT={port}")
    print("Target: existing RA4M1 application only; no Flash is written.")

    bridge: serial.Serial | None = None
    application_requested = False
    try:
        bridge = open_baseline_serial(port, 115200, 1.0)
        bridge.baudrate = 1200
        print("CDC_SET_LINE_CODING=1200")
        time.sleep(args.settle_ms / 1000.0)
        bridge.close()

        bridge = open_baseline_serial(port, 230400, 1.0)
        initialize_samba(bridge, 1.0, require_version=True)
        write_all(bridge, b"K#")
        application_requested = True
        print("SAMBA_RESET_COMMAND=K#")
        bridge.baudrate = 115200
        time.sleep(args.settle_ms / 1000.0)
        print("ESP32_RA4_SAMBA_RESET_AND_RUNTIME_PASS")
        return 0
    finally:
        if bridge is not None and bridge.is_open:
            if not application_requested:
                try:
                    write_all(bridge, b"K#")
                    print("SAMBA_FAILURE_RECOVERY=K#")
                except serial.SerialException:
                    print("SAMBA_FAILURE_RECOVERY=K#_SEND_FAILED", file=sys.stderr)
            bridge.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
