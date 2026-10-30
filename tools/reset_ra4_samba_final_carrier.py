#!/usr/bin/env python3
"""Reset and observe the paired RA4M1 through the final BL616CL carrier."""

from __future__ import annotations

import argparse
import sys
import time

import serial

from uno_r4_usb_identity import resolve_paired_bl616cl_port, tty_properties
from upload_ra4_samba_direct import capture_until_expected, initialize_samba
from upload_ra4_samba_split import open_serial, write_all


def require_final_carrier_bridge(port: str) -> None:
    properties = tty_properties(port)
    if (
        properties.get("ID_VENDOR_ID", "").lower() != "2341"
        or properties.get("ID_MODEL_ID", "").lower() != "1002"
        or properties.get("ID_USB_INTERFACE_NUM", "").lower() != "00"
        or properties.get("ID_SERIAL_SHORT") != "BL616CL-PROVISIONAL"
    ):
        raise RuntimeError(f"{port} is not the final-carrier BL616CL CDC interface")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--capture-ms", type=int, default=45000)
    parser.add_argument("--maximum-capture-bytes", type=int, default=16384)
    parser.add_argument("--runtime-settle-ms", type=int, default=300)
    parser.add_argument("--attempts", type=int, default=2)
    parser.add_argument("--retry-settle-ms", type=int, default=500)
    parser.add_argument("--expect", action="append", default=[])
    parser.add_argument(
        "--stop-when-expectations-met",
        action="store_true",
        help="Compatibility option; marker capture always stops when complete.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(port)
    if (
        args.capture_ms <= 0
        or args.maximum_capture_bytes <= 0
        or args.runtime_settle_ms < 0
        or args.attempts <= 0
        or args.retry_settle_ms < 0
    ):
        raise RuntimeError("invalid capture or retry settings")

    print(f"Bridge: {port} (final-carrier BL616CL 2341:1002)")
    print("Target: existing RA4M1 application only; no Flash is written.")

    last_error: BaseException | None = None
    for attempt in range(1, args.attempts + 1):
        bridge: serial.Serial | None = None
        runtime_requested = False
        try:
            print(f"RA4_FINAL_CARRIER_RESET_ATTEMPT={attempt}/{args.attempts}")
            bridge = open_serial(port, 115200, 1.0)
            bridge.baudrate = 1200
            print("CDC_SET_LINE_CODING=1200")
            time.sleep(0.5)
            bridge.close()
            bridge = open_serial(port, 230400, 1.0)
            initialize_samba(bridge, 1.0, require_version=False)
            write_all(bridge, b"K#")
            runtime_requested = True
            print("SAMBA_RESET_COMMAND=K#")
            bridge.baudrate = 115200
            time.sleep(args.runtime_settle_ms / 1000.0)
            runtime = capture_until_expected(
                bridge,
                args.capture_ms / 1000.0,
                args.maximum_capture_bytes,
                args.expect,
            )
            print("RA4_RUNTIME_CAPTURE_BEGIN")
            print(runtime, end="" if runtime.endswith("\n") else "\n")
            print("RA4_RUNTIME_CAPTURE_END")
            for marker in args.expect:
                if marker not in runtime:
                    raise RuntimeError(f"required runtime marker was not observed: {marker}")
            print("RA4_FINAL_CARRIER_SAMBA_RESET_AND_RUNTIME_PASS")
            return 0
        except (OSError, RuntimeError, serial.SerialException) as error:
            last_error = error
            print(
                f"RA4_FINAL_CARRIER_RESET_ATTEMPT_FAILED="
                f"{attempt}/{args.attempts}: {error}",
                file=sys.stderr,
            )
        finally:
            if bridge is not None and bridge.is_open:
                if not runtime_requested:
                    try:
                        write_all(bridge, b"K#")
                        print("SAMBA_FAILURE_RECOVERY=K#")
                    except serial.SerialException:
                        print("SAMBA_FAILURE_RECOVERY=K#_SEND_FAILED", file=sys.stderr)
                bridge.close()
        if attempt < args.attempts:
            time.sleep(args.retry_settle_ms / 1000.0)

    assert last_error is not None
    raise last_error


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
