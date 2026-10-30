#!/usr/bin/env python3
"""Probe the paired RA4M1 SAM-BA bootloader without changing application Flash."""

from __future__ import annotations

import argparse
import sys
import time

import serial

from ra4_relay_route import select_ra4_runtime_route
from uno_r4_usb_identity import resolve_paired_bl616cl_port, resolve_relay_port
from upload_ra4_samba_direct import (
    initialize_samba,
    open_serial,
    require_port_identity,
    restore_user_uart_baud,
    write_all,
)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument("--bootstrap-settle-ms", type=int, default=500)
    parser.add_argument("--runtime-settle-ms", type=int, default=300)
    parser.add_argument("--relay-settle-ms", type=int, default=500)
    parser.add_argument("--read-timeout-ms", type=int, default=1000)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.bootstrap_settle_ms <= 0
        or args.runtime_settle_ms < 0
        or args.relay_settle_ms < 0
        or args.read_timeout_ms <= 0
    ):
        raise RuntimeError("invalid timeout")

    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    args.relay_port = resolve_relay_port(args.relay_port)
    require_port_identity(args.bridge_port, "2341", "1002", "00")
    require_port_identity(args.relay_port, "0403", "6010", "00")
    print(f"Bridge: {args.bridge_port} (2341:1002 interface 00)")
    print(f"Relay:  {args.relay_port} (0403:6010 interface 00)")
    print("Target: RA4M1 bootloader probe only; no RA4M1, BL616CL, or ESP32-S3 Flash is written.")

    timeout = args.read_timeout_ms / 1000.0
    relay = serial.Serial(args.relay_port, 115200, timeout=0.2, write_timeout=0.2)
    relay.dtr = False
    relay.rts = False
    bridge: serial.Serial | None = None
    samba_may_be_active = False
    try:
        select_ra4_runtime_route(relay, args.relay_settle_ms)
        print(f"RELAY_RUNTIME_ROUTE=known-state settle_ms={args.relay_settle_ms}")

        bridge = open_serial(args.bridge_port, 115200, timeout)
        bridge.baudrate = 1200
        print("CDC_SET_LINE_CODING=1200")
        time.sleep(args.bootstrap_settle_ms / 1000.0)
        bridge.close()

        bridge = open_serial(args.bridge_port, 230400, timeout)
        samba_may_be_active = True
        version = initialize_samba(bridge, timeout, require_version=True)
        print(f"RA4_SAMBA_PROBE_VERSION={version}")
        print("RA4_SAMBA_PROBE_PASS")
        return 0
    finally:
        if samba_may_be_active and bridge is not None and bridge.is_open:
            try:
                write_all(bridge, b"K#")
                print("SAMBA_PROBE_RECOVERY=K#")
            except serial.SerialException:
                print("SAMBA_PROBE_RECOVERY=K#_SEND_FAILED", file=sys.stderr)
        if bridge is not None and bridge.is_open:
            bridge.close()
        try:
            restore_user_uart_baud(
                args.bridge_port,
                timeout,
                args.runtime_settle_ms / 1000.0,
            )
        except serial.SerialException:
            print("CDC_PROBE_RECOVERY=FAILED", file=sys.stderr)
        relay.rts = False
        relay.close()
        print("Relay state: RA4M1 UART runtime route retained (RTS# HIGH, relay released).")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
