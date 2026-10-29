#!/usr/bin/env python3
"""Run Arduino bossac through the final carrier's direct BL616CL CDC path.

The final carrier has no external split-UART relay: its BL616CL USB CDC is the
single full-duplex path to the RA4M1 user UART. bossac sends large SAM-BA
staging writes faster than the bridge's UART path can safely drain them, so a
PTY proxy preserves bossac's protocol while pacing only S-command payloads.
"""

from __future__ import annotations

import argparse
import os
import pty
import select
import subprocess
import termios
import time
import tty
from pathlib import Path

import serial

from run_ra4_bossac_direct import DirectSerialProxy
from upload_ra4_samba_split import open_serial
from uno_r4_usb_identity import resolve_paired_bl616cl_port, tty_properties


DEFAULT_BOSSAC = Path.home() / (
    ".arduino15/packages/arduino/tools/bossac/1.9.1-arduino5/bossac"
)


def require_final_carrier_bridge(port: str) -> None:
    properties = tty_properties(port)
    if (
        properties.get("ID_VENDOR_ID", "").lower() != "2341"
        or properties.get("ID_MODEL_ID", "").lower() != "1002"
        or properties.get("ID_USB_INTERFACE_NUM", "").lower() != "00"
        or properties.get("ID_SERIAL_SHORT") != "BL616CL-PROVISIONAL"
    ):
        raise RuntimeError(
            f"{port} is not the final-carrier BL616CL CDC interface"
        )


def uart_tx_snapshot() -> tuple[int, int, int]:
    """Read only counters from the identity-checked paired board's HID."""
    import inspect_bl616cl_gpio35 as hid
    fd = os.open(hid.find_hidraw(), os.O_RDWR)
    try:
        hid.select_feature_page(fd, 0)
        report = hid.read_feature(fd)
        if report[:5] != b'U35D' + bytes((hid.FEATURE_VERSION,)):
            raise RuntimeError("UART forwarding diagnostic signature mismatch")
        return tuple(int.from_bytes(report[i:i+4], 'little') for i in (40,36,44))
    finally:
        os.close(fd)


def wait_uart_forwarded(before: tuple[int, int, int], count: int,
                        timeout_seconds: float = 2.0) -> None:
    """USB flush completes before the bridge software queue is consumed.

    Once all bytes have reached Serial.write(), firmware updateBaudRate()
    flushes the hardware FIFO before changing baud. Do not switch earlier.
    """
    deadline = time.monotonic() + timeout_seconds
    while time.monotonic() < deadline:
        current = uart_tx_snapshot()
        if current[1:] != before[1:]:
            raise RuntimeError("UART forwarding drop/failure during SAM-BA upload")
        advanced = (current[0] - before[0]) & 0xffffffff
        if advanced == count:
            print("FINAL_CARRIER_BOSSAC_UART_FORWARDED=" + str(count), flush=True)
            return
        if advanced > count:
            raise RuntimeError("UART forwarding counter changed outside owned upload")
        time.sleep(.002)
    raise RuntimeError("SAM-BA handoff still pending in bridge UART software queue")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--bossac", type=Path, default=DEFAULT_BOSSAC)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--bootstrap-settle-ms", type=int, default=500)
    parser.add_argument("--runtime-settle-ms", type=int, default=500)
    parser.add_argument("--payload-packet-bytes", type=int, default=64)
    parser.add_argument("--payload-interpacket-delay-ms", type=int, default=10)
    parser.add_argument(
        "--attempts",
        type=int,
        default=3,
        help="Bounded full-image attempts; each attempt rewrites the same image.",
    )
    parser.add_argument("--retry-settle-ms", type=int, default=500)
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(args.bridge_port)

    bossac = args.bossac.resolve()
    image = args.image.resolve()
    if not bossac.is_file() or not os.access(bossac, os.X_OK):
        raise RuntimeError(f"bossac is not executable: {bossac}")
    if not image.is_file():
        raise RuntimeError(f"RA4M1 image does not exist: {image}")
    if (
        args.bootstrap_settle_ms <= 0
        or args.runtime_settle_ms < 0
        or args.payload_packet_bytes <= 0
        or args.payload_interpacket_delay_ms < 0
        or args.attempts <= 0
        or args.retry_settle_ms < 0
    ):
        raise RuntimeError("invalid upload pacing or timeout")

    print(f"Bridge: {args.bridge_port} (final-carrier BL616CL 2341:1002)")
    print(f"Image: {image}")
    print(
        "Target: RA4M1 application Flash only; BL616CL Flash is untouched."
    )
    print(
        "Transport: direct BL616CL USB CDC <-> GPIO34/GPIO35 <-> RA4M1 UART."
    )
    print("FINAL_CARRIER_BOSSAC_PAYLOAD_PACING="
          f"{args.payload_packet_bytes},{args.payload_interpacket_delay_ms},cross-read")

    last_error: BaseException | None = None
    for attempt in range(1, args.attempts + 1):
        bridge: serial.Serial | None = None
        master_fd = -1
        slave_fd = -1
        process: subprocess.Popen[str] | None = None
        try:
            print(f"FINAL_CARRIER_BOSSAC_UPLOAD_ATTEMPT={attempt}/{args.attempts}")
            bridge = open_serial(args.bridge_port, 115200, 1.0)
            bridge.baudrate = 1200
            print("CDC_SET_LINE_CODING=1200")
            time.sleep(args.bootstrap_settle_ms / 1000.0)
            bridge.close()

            bridge = open_serial(args.bridge_port, 230400, 1.0)
            bridge.timeout = 0
            master_fd, slave_fd = pty.openpty()
            tty.setraw(slave_fd)
            slave_name = os.ttyname(slave_fd)
            command = [
                str(bossac),
                "-d",
                "-U",
                f"--port={slave_name}",
                "-e",
                "-w",
                str(image),
                "-R",
            ]
            print("FINAL_CARRIER_BOSSAC_COMMAND=" + " ".join(command))
            before_forwarding = uart_tx_snapshot()
            process = subprocess.Popen(command, text=True)
            os.close(slave_fd)
            slave_fd = -1

            handoff_commands = []
            def observe_command(command: bytes) -> None:
                if command == b"K#":
                    handoff_commands.append(command)

            proxy = DirectSerialProxy(
                master_fd,
                bridge,
                args.payload_packet_bytes,
                args.payload_interpacket_delay_ms / 1000.0,
                command_observer=observe_command,
            )
            while process.poll() is None:
                proxy.pump_once(0.01)
            for _ in range(20):
                proxy.pump_once(0.01)
            if proxy.pending_bytes:
                raise RuntimeError(
                    f"bossac left {proxy.pending_bytes} forwarded byte(s)"
                )
            if process.returncode:
                raise RuntimeError(f"bossac exited with status {process.returncode}")
            if len(handoff_commands) != 1:
                raise RuntimeError("bossac application handoff was not forwarded exactly once")
            bridge.flush()
            wait_uart_forwarded(before_forwarding, proxy.forwarded_bytes)
            print("FINAL_CARRIER_BOSSAC_HANDOFF_FORWARDED")

            bridge.baudrate = 115200
            bridge.reset_input_buffer()
            print("CDC_SET_LINE_CODING=115200")
            time.sleep(args.runtime_settle_ms / 1000.0)
            print("FINAL_CARRIER_BOSSAC_UPLOAD_PASS")
            return 0
        except (
            OSError,
            RuntimeError,
            serial.SerialException,
            subprocess.SubprocessError,
        ) as error:
            last_error = error
            print(
                f"FINAL_CARRIER_BOSSAC_UPLOAD_ATTEMPT_FAILED="
                f"{attempt}/{args.attempts}: {error}"
            )
        finally:
            if process is not None and process.poll() is None:
                process.terminate()
                process.wait(timeout=5)
            if master_fd >= 0:
                os.close(master_fd)
            if slave_fd >= 0:
                os.close(slave_fd)
            if bridge is not None and bridge.is_open:
                bridge.close()

        if attempt < args.attempts:
            time.sleep(args.retry_settle_ms / 1000.0)

    assert last_error is not None
    raise last_error


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}")
        raise SystemExit(1)
