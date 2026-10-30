#!/usr/bin/env python3
"""Run Arduino bossac across the paired-board split RA4M1 UART paths.

The hand-wired board uses BL616CL GPIO34 only for host-to-RA4M1 traffic and
FTDI interface 00 only for RA4M1-to-host traffic.  bossac expects one duplex
serial port, so this tool supplies a pseudo-terminal and forwards its TX/RX
directions to the two physical ports.
"""

from __future__ import annotations

import argparse
import os
import pty
import re
import select
import subprocess
import sys
import termios
import time
import tty
from pathlib import Path

import serial


SAMBA_WRITE = re.compile(rb"^S[0-9A-F]{8},([0-9A-F]{8})#$")
DEFAULT_BOSSAC = Path.home() / (
    ".arduino15/packages/arduino/tools/bossac/1.9.1-arduino5/bossac"
)


def open_serial(port: str, baudrate: int, timeout: float) -> serial.Serial:
    link = serial.Serial(
        port,
        baudrate=baudrate,
        timeout=timeout,
        write_timeout=timeout,
    )
    link.dtr = False
    link.rts = False
    return link


def write_all(link: serial.Serial, payload: bytes) -> None:
    if payload:
        link.write(payload)
        link.flush()


def write_paced(
    link: serial.Serial,
    payload: bytes,
    packet_bytes: int,
    delay_seconds: float,
) -> None:
    for offset in range(0, len(payload), packet_bytes):
        write_all(link, payload[offset : offset + packet_bytes])
        if offset + packet_bytes < len(payload):
            time.sleep(delay_seconds)


class SplitSerialProxy:
    """Forward a bossac pseudo-terminal to the paired-board split UARTs."""

    def __init__(
        self,
        master_fd: int,
        bridge: serial.Serial,
        reference: serial.Serial,
        packet_bytes: int,
        delay_seconds: float,
    ) -> None:
        self._master_fd = master_fd
        self._bridge = bridge
        self._reference = reference
        self._packet_bytes = packet_bytes
        self._delay_seconds = delay_seconds
        self._tx_buffer = bytearray()
        self._write_payload_remaining = 0

    def pump_once(self, timeout: float) -> None:
        readable, _, _ = select.select(
            [self._master_fd, self._reference.fileno()],
            [],
            [],
            timeout,
        )
        if self._master_fd in readable:
            try:
                payload = os.read(self._master_fd, 4096)
            except OSError:
                payload = b""
            if payload:
                self._tx_buffer.extend(payload)
                self._flush_tx()

        if self._reference.fileno() in readable:
            # pyserial's read(size) waits for all requested bytes until its
            # timeout. During BOSSA's short auto-baud handshake that would
            # delay a two-byte bootloader reply by one full second. Drain only
            # bytes already queued by the FTDI driver and inject them into the
            # PTY immediately.
            payload = self._reference.read(max(1, self._reference.in_waiting))
            if payload:
                try:
                    os.write(self._master_fd, payload)
                except OSError:
                    pass

    def _flush_tx(self) -> None:
        while self._tx_buffer:
            if self._write_payload_remaining:
                count = min(self._write_payload_remaining, len(self._tx_buffer))
                payload = bytes(self._tx_buffer[:count])
                del self._tx_buffer[:count]
                write_paced(
                    self._bridge,
                    payload,
                    self._packet_bytes,
                    self._delay_seconds,
                )
                self._write_payload_remaining -= count
                continue

            # BOSSA sends each UART auto-baud 0x80 as an independent byte and
            # waits for the bootloader response before sending the next one.
            # It has no '#' command terminator, so it must bypass the normal
            # SAM-BA command framing below.
            if self._tx_buffer[0] == 0x80:
                write_all(self._bridge, b"\x80")
                del self._tx_buffer[0]
                continue

            command_end = self._tx_buffer.find(b"#")
            if command_end < 0:
                return
            command = bytes(self._tx_buffer[: command_end + 1])
            del self._tx_buffer[: command_end + 1]
            write_all(self._bridge, command)

            match = SAMBA_WRITE.match(command)
            if match is not None:
                self._write_payload_remaining = int(match.group(1), 16)
                # Keep command and binary payload in separate bridge transfers.
                time.sleep(0.002)

    @property
    def pending_payload_bytes(self) -> int:
        return self._write_payload_remaining + len(self._tx_buffer)


def trigger_bootloader(bridge: serial.Serial, settle_seconds: float) -> None:
    bridge.baudrate = 1200
    print("CDC_SET_LINE_CODING=1200")
    time.sleep(settle_seconds)
    bridge.close()


def capture(link: serial.Serial, duration_seconds: float, maximum: int) -> bytes:
    deadline = time.monotonic() + duration_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        payload = link.read(512)
        if payload:
            received.extend(payload)
            if len(received) > maximum:
                raise RuntimeError(f"runtime capture exceeded {maximum} bytes")
    return bytes(received)


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Run the Arduino bossac uploader across paired-board split UARTs."
    )
    parser.add_argument("--bridge-port", default="/dev/ttyACM1")
    parser.add_argument("--reference-port", default="/dev/ttyUSB0")
    parser.add_argument("--bossac", type=Path, default=DEFAULT_BOSSAC)
    parser.add_argument(
        "--image",
        type=Path,
        default=Path("build/ra4-uart-heartbeat/ra4-uart-heartbeat.ino.bin"),
    )
    parser.add_argument("--bootstrap-settle-ms", type=int, default=500)
    parser.add_argument(
        "--runtime-settle-ms",
        type=int,
        default=300,
        help="Wait after bossac reset and restoring 115200 baud before runtime checks.",
    )
    parser.add_argument("--runtime-capture-ms", type=int, default=3500)
    parser.add_argument("--payload-packet-bytes", type=int, default=64)
    parser.add_argument("--payload-interpacket-delay-ms", type=int, default=5)
    parser.add_argument(
        "--skip-runtime-smoke",
        action="store_true",
        help=(
            "Stop after a successful bossac upload. Use this for a Sketch with "
            "its own post-upload validation, such as the AT UART proxy."
        ),
    )
    parser.add_argument(
        "--info-only",
        action="store_true",
        help="Run bossac -d -U -i only; do not erase, write, or reset RA4M1 Flash.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.bootstrap_settle_ms <= 0
        or args.runtime_settle_ms < 0
        or args.runtime_capture_ms < 2000
        or args.payload_packet_bytes <= 0
        or args.payload_interpacket_delay_ms < 0
    ):
        raise RuntimeError("invalid timeout or payload pacing argument")

    bossac = args.bossac.resolve()
    if not bossac.is_file() or not os.access(bossac, os.X_OK):
        raise RuntimeError(f"bossac is not executable: {bossac}")
    image = args.image.resolve()
    if not args.info_only and not image.is_file():
        raise RuntimeError(f"compiled RA4M1 image does not exist: {image}")

    print(f"Bridge:    {args.bridge_port}")
    print(f"Reference: {args.reference_port}")
    print(f"Bossac:    {bossac}")
    if args.info_only:
        print("Target: RA4M1 bootloader information only. No Flash operation will be requested.")
    else:
        print(f"Image:     {image}")
        if args.skip_runtime_smoke:
            print(
                "Target: RA4M1 application Flash. bossac -U -e -w -R will erase, "
                "write, and reset the current Sketch; runtime verification is delegated."
            )
        else:
            print(
                "Target: RA4M1 application Flash. bossac -U -e -w -R will erase, "
                "write, and reset the current Sketch."
            )
    print("Relay route: RA4M1 UART (RTS=False, physical RTS# HIGH, relay released).")

    reference = open_serial(args.reference_port, 230400, 1.0)
    reference.timeout = 0
    bridge = open_serial(args.bridge_port, 115200, 1.0)
    master_fd = slave_fd = -1
    process: subprocess.Popen[str] | None = None
    try:
        reference.rts = False
        print(f"RELAY_RUNTIME_ROUTE rts={reference.rts}")
        trigger_bootloader(bridge, args.bootstrap_settle_ms / 1000)
        bridge = open_serial(args.bridge_port, 230400, 1.0)

        master_fd, slave_fd = pty.openpty()
        tty.setraw(slave_fd)
        slave_name = os.ttyname(slave_fd)
        command = [str(bossac), "-d", "-U", f"--port={slave_name}"]
        if args.info_only:
            command.append("-i")
        else:
            command.extend(["-e", "-w", str(image), "-R"])
        print("BOSSAC_COMMAND=" + " ".join(command))

        process = subprocess.Popen(command, text=True)
        os.close(slave_fd)
        slave_fd = -1

        proxy = SplitSerialProxy(
            master_fd,
            bridge,
            reference,
            args.payload_packet_bytes,
            args.payload_interpacket_delay_ms / 1000,
        )
        while process.poll() is None:
            proxy.pump_once(0.05)
        for _ in range(10):
            proxy.pump_once(0.01)
        if proxy.pending_payload_bytes:
            raise RuntimeError(
                f"bossac exited with {proxy.pending_payload_bytes} unforwarded TX byte(s)"
            )
        if process.returncode:
            raise RuntimeError(f"bossac exited with status {process.returncode}")
        print("BOSSAC_SPLIT_COMMAND_PASS")

        if args.info_only:
            return 0

        reference.reset_input_buffer()
        bridge.baudrate = 115200
        reference.baudrate = 115200
        time.sleep(args.runtime_settle_ms / 1000)

        if args.skip_runtime_smoke:
            print("RA4_BOSSAC_SPLIT_UPLOAD_PASS")
            return 0

        runtime = capture(reference, args.runtime_capture_ms / 1000, 8192)
        runtime_text = runtime.decode("ascii", errors="replace")
        print("RA4_RUNTIME_CAPTURE_BEGIN")
        print(runtime_text, end="" if runtime_text.endswith("\n") else "\n")
        print("RA4_RUNTIME_CAPTURE_END")
        if "RA4_HEARTBEAT:BOOT" not in runtime_text:
            raise RuntimeError("RA4M1 heartbeat boot marker was not observed after bossac reset")
        heartbeat_count = sum(
            1
            for line in runtime_text.splitlines()
            if line.startswith("RA4_HEARTBEAT:") and line != "RA4_HEARTBEAT:BOOT"
        )
        if heartbeat_count < 2:
            raise RuntimeError("fewer than two RA4M1 heartbeat markers were observed")

        nonce = f"bl616cl-bossac-{time.monotonic_ns():x}"
        reference.reset_input_buffer()
        write_all(bridge, f"{nonce}\n".encode("ascii"))
        echo_text = capture(reference, 1.0, 2048).decode("ascii", errors="replace")
        print(f"RA4_ECHO_CAPTURE={echo_text}")
        if f"RA4_ECHO:{nonce}" not in echo_text:
            raise RuntimeError(f"RA4M1 nonce echo was not observed: expected RA4_ECHO:{nonce}")
        print(f"RA4_BOSSAC_SPLIT_UPLOAD_AND_RUNTIME_PASS nonce={nonce}")
        return 0
    finally:
        if process is not None and process.poll() is None:
            process.terminate()
            process.wait(timeout=2)
        if master_fd >= 0:
            os.close(master_fd)
        if slave_fd >= 0:
            os.close(slave_fd)
        if bridge.is_open:
            bridge.close()
        reference.rts = False
        if reference.is_open:
            reference.close()
        print("Relay state: RA4M1 UART runtime route retained (RTS# HIGH).")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
