#!/usr/bin/env python3
"""Interact with the paired-board RA4M1 user UART over split TX/RX ports.

The BL616CL bridge CDC path sends to RA4M1 through GPIO34. RA4M1 output is
observed independently through the FTDI reference receiver because GPIO35 is
not yet a usable receive path on the hand-wired board.
"""

from __future__ import annotations

import argparse
import os
import select
import sys
import termios
import tty

import serial


def open_serial(port: str, baudrate: int) -> serial.Serial:
    link = serial.Serial(port, baudrate=baudrate, timeout=0, write_timeout=1)
    link.dtr = False
    link.rts = False
    return link


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Interactive RA4M1 split user-UART monitor."
    )
    parser.add_argument("--bridge-port", default="/dev/ttyACM1")
    parser.add_argument("--reference-port", default="/dev/ttyUSB0")
    parser.add_argument("--baud", type=int, default=115200)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if args.baud <= 0:
        raise RuntimeError("baud must be positive")
    if not sys.stdin.isatty():
        raise RuntimeError("interactive monitor requires a terminal")

    bridge: serial.Serial | None = None
    reference: serial.Serial | None = None
    stdin_fd = sys.stdin.fileno()
    saved_terminal = termios.tcgetattr(stdin_fd)
    try:
        bridge = open_serial(args.bridge_port, args.baud)
        reference = open_serial(args.reference_port, args.baud)
        # FTDI exposes RTS# (active low). False keeps RTS# high, releases the
        # relay, and selects the RA4M1 runtime return path.
        reference.rts = False
        reference.reset_input_buffer()
        print(
            f"RA4 split monitor: TX={args.bridge_port}, RX={args.reference_port}, "
            f"{args.baud} baud, relay runtime route (RTS=False)."
        )
        print("Press Ctrl-] to exit. Press Enter to send a newline to RA4M1.")
        tty.setcbreak(stdin_fd)

        while True:
            readable, _, _ = select.select(
                [stdin_fd, reference.fileno()],
                [],
                [],
                0.1,
            )
            if reference.fileno() in readable:
                payload = reference.read(max(1, reference.in_waiting))
                if payload:
                    os.write(sys.stdout.fileno(), payload)

            if stdin_fd not in readable:
                continue

            value = os.read(stdin_fd, 1)
            if value == b"\x1d":
                print("\nRA4 split monitor closed.")
                return 0
            if value in (b"\r", b"\n"):
                bridge.write(b"\n")
                bridge.flush()
                continue
            if value:
                bridge.write(value)
                bridge.flush()
    finally:
        termios.tcsetattr(stdin_fd, termios.TCSADRAIN, saved_terminal)
        if reference is not None and reference.is_open:
            reference.rts = False
            reference.close()
        if bridge is not None and bridge.is_open:
            bridge.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
