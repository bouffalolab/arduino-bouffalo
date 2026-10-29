#!/usr/bin/env python3
"""Capture credential-safe BL616CL console lines relevant to Wi-Fi disconnects."""

from __future__ import annotations

import argparse
import time

import serial


EVENT_TERMS = (
    "disassoc",
    "disconnect",
    "deauth",
    "event",
    "macsw",
    "wpa",
)
SENSITIVE_TERMS = ("exec ssid", "exec psk", "password", "credential")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="/dev/ttyUSB0")
    parser.add_argument("--baudrate", type=int, default=115200)
    parser.add_argument("--duration-seconds", type=float, default=90.0)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if args.baudrate <= 0 or args.duration_seconds <= 0:
        raise RuntimeError("baudrate and duration must be positive")

    deadline = time.monotonic() + args.duration_seconds
    pending = bytearray()
    print("BL616CL_CONSOLE_FILTER_STARTED")
    with serial.Serial(args.port, args.baudrate, timeout=0.2) as console:
        console.reset_input_buffer()
        while time.monotonic() < deadline:
            pending.extend(console.read(512))
            while b"\n" in pending:
                raw, _, pending = pending.partition(b"\n")
                line = raw.decode("utf-8", errors="replace").replace("\r", "")
                normalized = line.lower()
                if (
                    any(term in normalized for term in EVENT_TERMS)
                    and not any(term in normalized for term in SENSITIVE_TERMS)
                ):
                    print("BL616CL_CONSOLE_EVENT=" + line[:400])
    print("BL616CL_CONSOLE_FILTER_DONE")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException) as error:
        print(f"ERROR: {error}")
        raise SystemExit(1)
