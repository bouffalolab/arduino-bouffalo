#!/usr/bin/env python3
"""Run Arduino bossac through the direct BL616CL CDC UART path.

The PTY proxy preserves bossac's SAM-BA sequencing while pacing large
staging-buffer writes for the bridge's 512-byte CDC-to-UART queue.
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

from upload_ra4_samba_split import open_serial, write_all
from ra4_relay_route import select_ra4_runtime_route
from uno_r4_usb_identity import resolve_paired_bl616cl_port, resolve_relay_port


DEFAULT_BOSSAC = Path.home() / (
    ".arduino15/packages/arduino/tools/bossac/1.9.1-arduino5/bossac"
)


class DirectSerialProxy:
    def __init__(
        self,
        master_fd: int,
        link: serial.Serial,
        packet_bytes: int,
        delay_seconds: float,
        command_observer=None,
    ) -> None:
        self.master_fd = master_fd
        self.link = link
        self.packet_bytes = packet_bytes
        self.delay_seconds = delay_seconds
        self.tx_buffer = bytearray()
        self.payload_remaining = 0
        self.command_observer = command_observer
        self.next_payload_write_at = 0.0
        self.forwarded_bytes = 0

    def write_forwarded(self, data: bytes) -> None:
        if self.link.write(data) != len(data):
            raise serial.SerialTimeoutException("short SAM-BA proxy write")
        self.link.flush()
        self.forwarded_bytes += len(data)

    def pump_once(self, timeout: float) -> None:
        readable, _, _ = select.select([self.master_fd, self.link.fileno()], [], [], timeout)
        if self.master_fd in readable:
            try:
                payload = os.read(self.master_fd, 4096)
            except OSError:
                payload = b""
            if payload:
                self.tx_buffer.extend(payload)
                self.flush_tx()

        if self.link.fileno() in readable:
            payload = self.link.read(max(1, self.link.in_waiting))
            if payload:
                try:
                    os.write(self.master_fd, payload)
                except OSError:
                    pass

    def flush_tx(self) -> None:
        while self.tx_buffer:
            if self.payload_remaining:
                count = min(self.payload_remaining, len(self.tx_buffer))
                payload = bytes(self.tx_buffer[:count])
                del self.tx_buffer[:count]
                for offset in range(0, len(payload), self.packet_bytes):
                    # PTY reads can split an S payload into separate 64-byte
                    # chunks. Keep pacing across reads, not just within one
                    # flush_tx call, or those chunks bypass the drain budget.
                    remaining = self.next_payload_write_at - time.monotonic()
                    if remaining > 0:
                        time.sleep(remaining)
                    self.write_forwarded(payload[offset : offset + self.packet_bytes])
                    self.next_payload_write_at = time.monotonic() + self.delay_seconds
                self.payload_remaining -= count
                continue

            if self.tx_buffer[0] == 0x80:
                self.write_forwarded(b"\x80")
                del self.tx_buffer[0]
                continue

            command_end = self.tx_buffer.find(b"#")
            if command_end < 0:
                return
            command = bytes(self.tx_buffer[: command_end + 1])
            del self.tx_buffer[: command_end + 1]
            self.write_forwarded(command)
            if self.command_observer is not None:
                self.command_observer(command)
            if command.startswith(b"S") and command.endswith(b"#"):
                try:
                    self.payload_remaining = int(command[10:18], 16)
                except ValueError:
                    self.payload_remaining = 0
                time.sleep(0.002)

    @property
    def pending_bytes(self) -> int:
        return self.payload_remaining + len(self.tx_buffer)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--relay-port", default="auto")
    parser.add_argument("--bossac", type=Path, default=DEFAULT_BOSSAC)
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--bootstrap-settle-ms", type=int, default=500)
    parser.add_argument("--runtime-settle-ms", type=int, default=500)
    parser.add_argument(
        "--relay-settle-ms",
        type=int,
        default=500,
        help="Known-route settle time after selecting the RA4M1 relay path.",
    )
    parser.add_argument("--payload-packet-bytes", type=int, default=64)
    parser.add_argument("--payload-interpacket-delay-ms", type=int, default=10)
    parser.add_argument(
        "--attempts",
        type=int,
        default=1,
        help="Number of bounded full-image upload attempts.",
    )
    parser.add_argument(
        "--retry-settle-ms",
        type=int,
        default=500,
        help="Delay between failed full-image upload attempts.",
    )
    parser.add_argument(
        "--usbipd-busid",
        help="When using WSL2 USB/IP, reattach this physical BusId after the 1200-baud reset.",
    )
    parser.add_argument(
        "--usbipd-distribution",
        default="Ubuntu-22.04",
        help="WSL distribution passed to usbipd when --usbipd-busid is used.",
    )
    parser.add_argument(
        "--relay-already-runtime",
        action="store_true",
        help="Keep the existing runtime relay state; do not pulse RTS before opening the bridge.",
    )
    parser.add_argument(
        "--skip-runtime-smoke",
        action="store_true",
        help=argparse.SUPPRESS,
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    if not args.relay_already_runtime:
        args.relay_port = resolve_relay_port(args.relay_port)
    bossac = args.bossac.resolve()
    image = args.image.resolve()
    if not bossac.is_file() or not os.access(bossac, os.X_OK):
        raise RuntimeError(f"bossac is not executable: {bossac}")
    if not image.is_file():
        raise RuntimeError(f"RA4M1 image does not exist: {image}")
    if (
        args.payload_packet_bytes <= 0
        or args.payload_interpacket_delay_ms < 0
        or args.attempts <= 0
        or args.retry_settle_ms < 0
        or args.relay_settle_ms < 0
    ):
        raise RuntimeError("invalid payload pacing")

    relay = None
    if not args.relay_already_runtime:
        relay = serial.Serial(args.relay_port, 115200, timeout=0.2, write_timeout=0.2)
        relay.dtr = False
        relay.rts = False
    last_error: BaseException | None = None
    try:
        print(f"Bridge: {args.bridge_port} (BL616CL 2341:1002)")
        print(f"Relay: {args.relay_port} (FTDI 0403:6010, RTS=False runtime route)")
        print(f"Image: {image}")
        print("Target: RA4M1 application Flash only; BL616CL and ESP32-S3 Flash untouched.")
        for attempt in range(1, args.attempts + 1):
            bridge = None
            master_fd = slave_fd = -1
            process = None
            try:
                print(f"BOSSAC_DIRECT_UPLOAD_ATTEMPT={attempt}/{args.attempts}")
                if args.relay_already_runtime:
                    print("RELAY_RUNTIME_ROUTE=already-runtime")
                else:
                    assert relay is not None
                    select_ra4_runtime_route(relay, args.relay_settle_ms)
                    print(
                        "RELAY_RUNTIME_ROUTE="
                        f"known-state settle_ms={args.relay_settle_ms}"
                    )
                bridge = open_serial(args.bridge_port, 115200, 1.0)
                bridge.baudrate = 1200
                print("CDC_SET_LINE_CODING=1200")
                time.sleep(args.bootstrap_settle_ms / 1000)
                bridge.close()
                if args.usbipd_busid:
                    reattach = [
                        "usbipd.exe",
                        "wsl",
                        "attach",
                        "--busid",
                        args.usbipd_busid,
                        "--distribution",
                        args.usbipd_distribution,
                    ]
                    print("USBIPD_REATTACH=" + " ".join(reattach))
                    result = subprocess.run(
                        reattach,
                        check=False,
                        timeout=30,
                        capture_output=True,
                        text=True,
                    )
                    if result.returncode:
                        detach = ["usbipd.exe", "wsl", "detach", "--busid", args.usbipd_busid]
                        subprocess.run(detach, check=False, timeout=15, capture_output=True, text=True)
                        result = subprocess.run(
                            reattach,
                            check=False,
                            timeout=30,
                            capture_output=True,
                            text=True,
                        )
                        if result.returncode:
                            detail = (result.stdout + result.stderr).strip()
                            raise RuntimeError(
                                f"usbipd reattach failed with status {result.returncode}: {detail}"
                            )
                    time.sleep(1.0)
                bridge = open_serial(args.bridge_port, 230400, 1.0)
                # The PTY proxy must forward bootloader replies as soon as
                # select() reports them. A blocking read here can consume the
                # full serial timeout after a short SAM-BA auto-baud reply.
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
                print("BOSSAC_DIRECT_THROTTLED_COMMAND=" + " ".join(command))
                process = subprocess.Popen(command, text=True)
                os.close(slave_fd)
                slave_fd = -1

                proxy = DirectSerialProxy(
                    master_fd,
                    bridge,
                    args.payload_packet_bytes,
                    args.payload_interpacket_delay_ms / 1000,
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
                    raise RuntimeError(
                        f"bossac exited with status {process.returncode}"
                    )
                print("BOSSAC_DIRECT_THROTTLED_UPLOAD_PASS")

                # bossac uploads at the SAM-BA 230400 baud. Its reset starts
                # the Sketch, whose user UART is 115200, so restore the CDC
                # line coding before the caller opens its runtime session.
                bridge.baudrate = 115200
                bridge.reset_input_buffer()
                print("CDC_SET_LINE_CODING=115200")
                time.sleep(args.runtime_settle_ms / 1000)
                print("RA4M1 runtime route retained (RTS# HIGH).")
                return 0
            except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
                last_error = error
                print(
                    f"BOSSAC_DIRECT_UPLOAD_ATTEMPT_FAILED={attempt}/{args.attempts}: {error}"
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
                time.sleep(args.retry_settle_ms / 1000)

        assert last_error is not None
        raise last_error
    finally:
        if relay is not None:
            relay.rts = False
            relay.close()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}")
        raise SystemExit(1)
