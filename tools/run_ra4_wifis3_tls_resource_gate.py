#!/usr/bin/env python3
"""Run WiFiSSLClient slot exhaustion and reuse on the final BL616CL carrier.

The temporary RA4M1 Sketch keeps eight TLS clients connected, proves a ninth
allocation is rejected, closes a middle slot, verifies an intentional
certificate failure can be closed, and reuses that same raw TLS slot for an
application-data exchange. Credentials and generated PKI material stay only
in process memory or an ignored temporary build directory.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import ipaddress
import json
import os
import secrets
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_bl616cl_apsta_gate import DEFAULT_CREDENTIAL_ENV, load_private_credentials
import run_ra4_wifis3_scan_final_carrier as final_scan
from run_ra4_wifis3_tls_gate import (
    configure_peer_firewall,
    create_material,
    pem_c_literal,
    read_peer_event_log,
    remove_peer_firewall,
    wait_for_peer_events,
    windows_wlan_address,
    wsl_unc_path,
)
from bounded_serial_close import close_serial_bounded
from uno_r4_usb_identity import resolve_paired_bl616cl_port
from bl616cl_usb_mapping import ensure_bl_usb_mapping, stale_bl_ports


ROOT_DIR = Path(__file__).resolve().parents[1]
FQBN = "arduino:renesas_uno:unor4wifi"
TEMPLATE = (
    ROOT_DIR
    / "examples"
    / "ra4-wifis3-tls-resource-gate"
    / "ra4-wifis3-tls-resource-gate.ino.in"
)
WORK_ROOT = ROOT_DIR / "build" / "ra4-wifis3-tls-resource-gate"
OVERLAY_PREPARER = (
    ROOT_DIR / "tools" / "prepare_wifis3_result_error_overlay.py"
)
SAMBA_RUNNER = ROOT_DIR / "tools" / "run_ra4_bossac_final_carrier.py"
TLS_HOSTNAME = "tls.bl616cl.test"
TLS_PORT = 18444
MIXED_TCP_PORT = 18445
MIXED_SERVER_PORT = 25081


WINDOWS_RESOURCE_PEER = r"""
import argparse
import ctypes
import os
from pathlib import Path
import sys
import socket
import ssl
import struct
import threading
import time

EXPECTED_HOSTNAME = "tls.bl616cl.test"
EXPECTED_REQUEST = b"bl616cl-tls-resource-ping"
EXPECTED_REPLY = b"bl616cl-tls-resource-echo"
EVENT_LOG_LOCK = threading.Lock()


def emit(message, event_log):
    with EVENT_LOG_LOCK:
        with open(event_log, "a", encoding="ascii") as output:
            output.write(message + "\n")
    # Readiness is consumed from stdout; all later evidence is read from the
    # event file. Long runs must not block network workers on an undrained pipe.
    if message == "TLS_RESOURCE_PEER:READY":
        print(message, flush=True)


def monitor_tcp(connection, stop, index, event_log):
    marker = "TLS_RESOURCE_PEER"
    # The launcher does not continuously drain stdout after READY. Keep these
    # periodic samples in the event file so diagnostics cannot fill that pipe.
    def record(message):
        with EVENT_LOG_LOCK:
            with open(event_log, "a", encoding="ascii") as output:
                output.write(message + "\n")

    if sys.platform != "win32":
        return
    # Windows TCP_INFO_v0, queried read-only using SIO_TCP_INFO. No payload,
    # endpoint address or request text is included in this diagnostic.
    class TcpInfo(ctypes.Structure):
        _fields_ = [
            ("State", ctypes.c_uint32), ("Mss", ctypes.c_uint32),
            ("ConnectionTimeMs", ctypes.c_uint64),
            ("TimestampsEnabled", ctypes.c_ubyte),
            *[(name, ctypes.c_uint32) for name in (
                "RttUs", "MinRttUs", "BytesInFlight", "Cwnd", "SndWnd",
                "RcvWnd", "RcvBuf")],
            ("BytesOut", ctypes.c_uint64), ("BytesIn", ctypes.c_uint64),
            *[(name, ctypes.c_uint32) for name in (
                "BytesReordered", "BytesRetrans", "FastRetrans", "DupAcksIn",
                "TimeoutEpisodes")],
            ("SynRetrans", ctypes.c_ubyte),
        ]
    winsock = ctypes.WinDLL("ws2_32", use_last_error=True)
    ioctl = winsock.WSAIoctl
    ioctl.argtypes = [ctypes.c_size_t, ctypes.c_uint32, ctypes.c_void_p,
                     ctypes.c_uint32, ctypes.c_void_p, ctypes.c_uint32,
                     ctypes.c_void_p, ctypes.c_void_p, ctypes.c_void_p]
    ioctl.restype = ctypes.c_int
    while not stop.is_set():
        version, returned, info = ctypes.c_uint32(0), ctypes.c_uint32(), TcpInfo()
        result = ioctl(connection.fileno(), 0xD8000027, ctypes.byref(version),
                       ctypes.sizeof(version), ctypes.byref(info), ctypes.sizeof(info),
                       ctypes.byref(returned), None, None)
        if result != 0 or returned.value < ctypes.sizeof(info):
            record(marker + ":TCP_INFO_UNAVAILABLE=" + str(index) + ","
                   + str(winsock.WSAGetLastError()))
            return
        record(marker + ":TCP_INFO=" + str(index) + "," + ",".join(
            name + ":" + str(getattr(info, name)) for name in (
                "State", "Mss", "BytesInFlight", "Cwnd", "SndWnd",
                "BytesOut", "BytesRetrans", "DupAcksIn", "TimeoutEpisodes")))
        stop.wait(1)


def decode_dns_name(packet, offset):
    labels = []
    while True:
        if offset >= len(packet):
            raise ValueError("DNS name exceeds packet")
        length = packet[offset]
        offset += 1
        if length == 0:
            return ".".join(labels), offset
        if length & 0xC0 or offset + length > len(packet):
            raise ValueError("unsupported DNS query")
        labels.append(packet[offset:offset + length].decode("ascii"))
        offset += length


def dns_server(bind_ip, stop, ready, event_log):
    link = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    link.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    link.bind((bind_ip, 53))
    link.settimeout(0.2)
    ready.set()
    try:
        while not stop.is_set():
            try:
                packet, client = link.recvfrom(512)
            except socket.timeout:
                continue
            try:
                question, offset = decode_dns_name(packet, 12)
                qtype, qclass = struct.unpack("!HH", packet[offset:offset + 4])
                question_end = offset + 4
                if question != EXPECTED_HOSTNAME or qtype != 1 or qclass != 1:
                    continue
                response = bytearray(packet[:2])
                response += struct.pack("!HHHHH", 0x8180, 1, 1, 0, 0)
                response += packet[12:question_end]
                response += b"\xc0\x0c"
                response += struct.pack("!HHIH", 1, 1, 30, 4)
                response += socket.inet_aton(bind_ip)
                link.sendto(response, client)
                emit("TLS_RESOURCE_PEER:DNS_QUERY", event_log)
            except (IndexError, OSError, UnicodeError, ValueError):
                emit("TLS_RESOURCE_PEER:DNS_ERROR", event_log)
    finally:
        link.close()


def tls_server(bind_ip, port, cert, key, event_log, stop, ready, rounds, queue_recovery=False, receive_burst_bytes=0, tcp_diagnostics=False):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile=cert, keyfile=key)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.set_ciphers("ECDHE-ECDSA-AES128-GCM-SHA256")
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((bind_ip, port))
    listener.listen(16)
    listener.settimeout(0.2)
    sequence = {"next": 0}
    sequence_lock = threading.Lock()
    reuse_count = {"value": 0}
    reuse_lock = threading.Lock()

    def handle(connection, index):
        with connection:
            monitor_stop = threading.Event()
            monitor = None
            phase = "handshake"
            try:
                connection.settimeout(15.0)
                with context.wrap_socket(connection, server_side=True) as secure:
                    phase = "established"
                    emit(f"TLS_RESOURCE_PEER:HANDSHAKE_OK={index}", event_log)
                    secure.settimeout(1.0)
                    if index % 10 == 0:
                        request = secure.recv(128)
                        if request == EXPECTED_REQUEST:
                            secure.sendall(EXPECTED_REPLY)
                            with reuse_lock:
                                reuse_count["value"] += 1
                                completed_rounds = reuse_count["value"]
                            emit(
                                "TLS_RESOURCE_PEER:REUSE_EXCHANGE_PASS="
                                f"{completed_rounds}",
                                event_log,
                            )
                        else:
                            emit("TLS_RESOURCE_PEER:REUSE_EXCHANGE_REJECTED", event_log)
                        return
                    while not stop.is_set():
                        try:
                            data = secure.recv(256)
                            if not data:
                                return
                            burst_request = b"bl616cl-rx-burst"
                            if receive_burst_bytes and burst_request.startswith(data):
                                while len(data) < len(burst_request):
                                    part = secure.recv(len(burst_request) - len(data))
                                    if not part:
                                        return
                                    data += part
                                if data != burst_request:
                                    emit(f"TLS_RESOURCE_PEER:RX_BURST_REJECT={index}", event_log)
                                    continue
                                slot = (index - 1) % 10
                                secure.settimeout(15.0)
                                if tcp_diagnostics and monitor is None:
                                    monitor = threading.Thread(
                                        target=monitor_tcp,
                                        args=(secure, monitor_stop, index, event_log),
                                        daemon=True,
                                    )
                                    monitor.start()
                                secure.sendall(bytes((slot + offset * 19) & 255
                                                     for offset in range(receive_burst_bytes)))
                                emit(f"TLS_RESOURCE_PEER:RX_BURST_SENT={index},{receive_burst_bytes}", event_log)
                                secure.settimeout(1.0)
                                continue
                            if data.startswith(b"bl616cl-queue-hold"):
                                emit(f"TLS_RESOURCE_PEER:QUEUE_HOLD={index}", event_log)
                                # Keep the connection alive without draining application
                                # data; the small receive window creates real backpressure.
                                stop.wait(60.0)
                                return
                        except socket.timeout:
                            continue
            except ssl.SSLError as error:
                emit(f"TLS_RESOURCE_PEER:FAILURE_DETAIL={index},{phase},SSL,{error.errno or 0},"
                     f"{getattr(error, 'reason', None) or 'UNKNOWN'}", event_log)
                emit(f"TLS_RESOURCE_PEER:HANDSHAKE_REJECTED={index}", event_log)
            except OSError as error:
                emit(f"TLS_RESOURCE_PEER:FAILURE_DETAIL={index},{phase},OS,{error.errno or 0},"
                     f"{getattr(error, 'winerror', None) or 0}", event_log)
                emit(f"TLS_RESOURCE_PEER:TCP_ERROR={index}", event_log)
                # A client that rejects the intentionally untrusted CA can
                # reset the connection before Python raises SSLError.
                emit(f"TLS_RESOURCE_PEER:HANDSHAKE_REJECTED={index}", event_log)

            finally:
                monitor_stop.set()
                if monitor is not None:
                    monitor.join(timeout=2.0)

    ready.set()
    try:
        while not stop.is_set():
            with reuse_lock:
                if reuse_count["value"] >= rounds:
                    break
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                continue
            with sequence_lock:
                sequence["next"] += 1
                index = sequence["next"]
            if queue_recovery and index % 10 == 1:
                connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
            emit(f"TLS_RESOURCE_PEER:TCP_ACCEPTED={index}", event_log)
            threading.Thread(target=handle, args=(connection, index), daemon=True).start()
    finally:
        listener.close()


def mixed_tcp_server(bind_ip, port, server_port, event_log, stop, ready):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((bind_ip, port))
    listener.listen(2)
    listener.settimeout(0.2)
    ready.set()

    def connect_inbound(board_ip):
        deadline = time.monotonic() + 15
        attempts = 0
        last_error = None
        while time.monotonic() < deadline and not stop.is_set():
            attempts += 1
            try:
                with socket.create_connection((board_ip, server_port), timeout=1) as link:
                    emit("TLS_RESOURCE_PEER:MIXED_SERVER_CONNECTED", event_log)
                    link.settimeout(0.2)
                    while not stop.is_set():
                        try:
                            if not link.recv(128):
                                return
                        except socket.timeout:
                            continue
                    return
            except OSError as error:
                last_error = error
                time.sleep(0.1)
        emit(f"TLS_RESOURCE_PEER:MIXED_SERVER_FAILURE_DETAIL={attempts},"
             f"{getattr(last_error, 'errno', None) or 0},"
             f"{getattr(last_error, 'winerror', None) or 0}", event_log)
        emit("TLS_RESOURCE_PEER:MIXED_SERVER_CONNECT_FAILED", event_log)

    try:
        while not stop.is_set():
            try:
                connection, source = listener.accept()
            except socket.timeout:
                continue
            emit("TLS_RESOURCE_PEER:MIXED_TCP_ACCEPTED", event_log)
            threading.Thread(
                target=connect_inbound,
                args=(source[0],),
                daemon=True,
            ).start()
            with connection:
                connection.settimeout(0.2)
                while not stop.is_set():
                    try:
                        if not connection.recv(128):
                            break
                    except socket.timeout:
                        continue
    finally:
        listener.close()


def all_tcp_slots_server(bind_ip, port, server_port, event_log, stop, ready):
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((bind_ip, port))
    listener.listen(16)
    listener.settimeout(0.2)
    workers = []
    counts = {"PLAIN": 0, "ACCEPTED": 0}
    counts_lock = threading.Lock()
    payload = bytes((0x5a + i*19) & 255 for i in range(64))

    def serve(link, kind):
        with link:
            link.settimeout(0.2)
            try:
                while not stop.is_set():
                    try:
                        request = link.recv(1)
                        break
                    except socket.timeout:
                        continue
                else:
                    return
                if request != b"X":
                    emit("TLS_RESOURCE_PEER:ALL_TCP_REQUEST_REJECTED=" + kind, event_log)
                    return
                link.sendall(payload)
                with counts_lock:
                    counts[kind] += 1
                    index = counts[kind]
                emit(f"TLS_RESOURCE_PEER:ALL_TCP_{kind}_CONTENT_SENT={index},64", event_log)
                # Hold the slot until the RA4 test closes it, even after sending.
                while not stop.is_set():
                    try:
                        if not link.recv(1):
                            return
                        emit("TLS_RESOURCE_PEER:ALL_TCP_EXTRA_DATA=" + kind, event_log)
                        return
                    except socket.timeout:
                        continue
            except OSError:
                emit("TLS_RESOURCE_PEER:ALL_TCP_SOCKET_ERROR=" + kind, event_log)

    def inbound(board_ip):
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline and not stop.is_set():
            try:
                link = socket.create_connection((board_ip, server_port), timeout=1)
                serve(link, "ACCEPTED")
                return
            except OSError:
                time.sleep(0.1)
        emit("TLS_RESOURCE_PEER:ALL_TCP_ACCEPT_CONNECT_FAILED", event_log)

    ready.set()
    try:
        while not stop.is_set():
            try:
                link, source = listener.accept()
            except socket.timeout:
                continue
            for target, args in ((serve, (link, "PLAIN")), (inbound, (source[0],))):
                worker = threading.Thread(target=target, args=args, daemon=True)
                workers.append(worker)
                worker.start()
    finally:
        listener.close()
        for worker in workers:
            worker.join(timeout=1)


def ipv4_reassembly_statistics():
    import os
    if os.name != "nt":
        return None
    import ctypes
    # MIB_IPSTATS has 23 DWORD fields. These are the host-wide IPv4
    # reassembly requests/successes/failures, with no packet contents.
    statistics = (ctypes.c_uint32 * 23)()
    if ctypes.WinDLL("iphlpapi").GetIpStatistics(ctypes.byref(statistics)) != 0:
        return None
    return tuple(statistics[index] for index in (14, 15, 16))


def mixed_udp_server(bind_ip, port, rounds, event_log, stop, ready, varied_payload=False):
    seen = set()
    previous_reassembly = None
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as link:
        link.bind((bind_ip, port))
        link.settimeout(0.2)
        ready.set()
        while not stop.is_set():
            reassembly = ipv4_reassembly_statistics()
            if reassembly is not None and reassembly != previous_reassembly:
                emit("TLS_RESOURCE_PEER:MIXED_HOST_IP_REASSEMBLY=" +
                     ",".join(str(value) for value in reassembly), event_log)
                previous_reassembly = reassembly
            try:
                packet, source = link.recvfrom(65535)
            except socket.timeout:
                continue
            if len(packet) != 16384:
                emit("TLS_RESOURCE_PEER:MIXED_UDP_REJECT_LENGTH=" + str(len(packet)), event_log)
                continue
            sequence = int.from_bytes(packet[:4], "little")
            if not 1 <= sequence <= rounds:
                emit("TLS_RESOURCE_PEER:MIXED_UDP_REJECT_SEQUENCE=" + str(sequence), event_log)
                continue
            expected = (bytes((sequence - 1 + index * 19) & 255 for index in range(4, 16384))
                        if varied_payload else bytes(16380))
            if packet[4:] != expected:
                emit("TLS_RESOURCE_PEER:MIXED_UDP_REJECT_PAYLOAD", event_log)
                continue
            if sequence not in seen:
                seen.add(sequence)
                emit("TLS_RESOURCE_PEER:MIXED_UDP_16K_RECEIVED=" + str(sequence), event_log)
            link.sendto(packet[:4], source)


def main():
    Path(__file__).with_suffix('.pid').write_text(str(os.getpid()), encoding='ascii')
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind-ip", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--event-log", required=True)
    parser.add_argument("--rounds", required=True, type=int)
    parser.add_argument("--all-tcp-slots", action="store_true")
    parser.add_argument("--mixed-tcp-port", type=int)
    parser.add_argument("--mixed-server-port", type=int)
    parser.add_argument("--mixed-udp-port", type=int)
    parser.add_argument("--mixed-udp-varied-payload", action="store_true")
    parser.add_argument("--sta-queue-recovery", action="store_true")
    parser.add_argument("--tcp-diagnostics", action="store_true")
    parser.add_argument("--receive-burst-bytes", type=int, default=0)
    args = parser.parse_args()
    if args.rounds <= 0:
        raise RuntimeError("round count must be positive")
    if not 0 <= args.receive_burst_bytes <= 16384:
        raise RuntimeError("invalid receive burst size")
    stop = threading.Event()
    dns_ready = threading.Event()
    tls_ready = threading.Event()
    mixed_ready = threading.Event()
    udp_ready = threading.Event()
    dns = threading.Thread(
        target=dns_server,
        args=(args.bind_ip, stop, dns_ready, args.event_log),
        daemon=True,
    )
    tls = threading.Thread(
        target=tls_server,
        args=(
            args.bind_ip,
            args.port,
            args.cert,
            args.key,
            args.event_log,
            stop,
            tls_ready,
            args.rounds,
            args.sta_queue_recovery,
            args.receive_burst_bytes,
            args.tcp_diagnostics,
        ),
        daemon=True,
    )
    dns.start()
    tls.start()
    mixed = None
    if args.mixed_tcp_port is not None:
        mixed = threading.Thread(
            target=all_tcp_slots_server if args.all_tcp_slots else mixed_tcp_server,
            args=(args.bind_ip, args.mixed_tcp_port, args.mixed_server_port,
                  args.event_log, stop, mixed_ready),
            daemon=True,
        )
        mixed.start()
    udp = None
    if args.mixed_udp_port is not None:
        udp = threading.Thread(
            target=mixed_udp_server,
            args=(args.bind_ip, args.mixed_udp_port, args.rounds,
                  args.event_log, stop, udp_ready, args.mixed_udp_varied_payload), daemon=True,
        )
        udp.start()
    if (not dns_ready.wait(5) or not tls_ready.wait(5) or
            (mixed is not None and not mixed_ready.wait(5)) or
            (udp is not None and not udp_ready.wait(5))):
        raise RuntimeError("peer startup timed out")
    emit("TLS_RESOURCE_PEER:READY", args.event_log)
    deadline = time.monotonic() + max(90, args.rounds * 45)
    while time.monotonic() < deadline:
        with open(args.event_log, "r", encoding="ascii") as input_file:
            if (
                "TLS_RESOURCE_PEER:REUSE_EXCHANGE_PASS="
                f"{args.rounds}" in input_file.read()
            ):
                break
        time.sleep(0.1)
    stop.set()
    tls.join(timeout=3)
    dns.join(timeout=3)
    if mixed is not None:
        mixed.join(timeout=3)
    if udp is not None:
        udp.join(timeout=3)


if __name__ == "__main__":
    main()
"""


class InterruptionInjected(Exception):
    """Finish the injection path while retaining the common cleanup checks."""


class MarkerCaptureError(RuntimeError):
    def __init__(self, marker: str, captured: str) -> None:
        super().__init__(f"RA4M1 TLS resource gate failed: {marker}")
        self.captured = captured


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def encode_start_value(value: str) -> str:
    return value.encode("ascii", "strict").hex()


def timeline(stage: str) -> None:
    print(f"TLS_RESOURCE_TIMELINE={datetime.now(timezone.utc).isoformat()},{stage}", flush=True)


def capture_until(
    link: serial.Serial,
    required: tuple[str, ...],
    timeout_seconds: float,
    idle_timeout_seconds: float | None = None,
    byte_limit: int = 65536,
    usb_echo: bool = False,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    last_progress = time.monotonic()
    received = bytearray()
    echoed_rounds = set()
    observed_end = 0
    while time.monotonic() < deadline:
        try:
            payload = link.read(512)
        except (OSError, serial.SerialException) as error:
            raise MarkerCaptureError(
                "RA4_TLS_RESOURCE:USB_READ_ERROR:" + type(error).__name__,
                received.decode("ascii", errors="replace").replace("\x00", ""),
            ) from error
        if not payload:
            if idle_timeout_seconds is not None and time.monotonic() - last_progress >= idle_timeout_seconds:
                raise MarkerCaptureError("RA4_TLS_RESOURCE:PROGRESS_TIMEOUT",
                                         received.decode("ascii", errors="replace").replace("\x00", ""))
            continue
        last_progress = time.monotonic()
        received.extend(payload)
        if len(received) > byte_limit:
            raise MarkerCaptureError(
                "RA4_TLS_RESOURCE:OUTPUT_LIMIT",
                received.decode("ascii", errors="replace").replace("\x00", ""),
            )
        text = received.decode("ascii", errors="replace").replace("\x00", "")
        complete_end = text.rfind("\n") + 1
        for line in text[observed_end:complete_end].splitlines():
            if re.match(r"RA4_TLS_RESOURCE:(?:EIGHT_FAILURE_|ROUND_FAILURE_|MIXED_.*FAILURE|MIXED_UDP_ACK_STATE)", line):
                timeline("SERIAL_" + line.split(":", 1)[1].split("=", 1)[0])
        observed_end = complete_end
        failure = "RA4_TLS_RESOURCE:RESULT=FAIL:"
        if failure in text:
            start = text.find(failure)
            end = text.find("\n", start)
            timeline("RESULT_FAIL_OBSERVED")
            raise MarkerCaptureError(
                text[start:end if end >= 0 else len(text)].strip(),
                text,
            )
        if usb_echo:
            for value in re.findall(r"RA4_TLS_RESOURCE:USB_ECHO_READY=(\d+)\r?\n", text):
                round_number = int(value)
                if round_number not in echoed_rounds:
                    payload = f"USB {round_number}\n".encode("ascii")
                    try:
                        if link.write(payload) != len(payload):
                            raise MarkerCaptureError("RA4_TLS_RESOURCE:USB_SHORT_WRITE", text)
                        link.flush()
                    except (OSError, serial.SerialException) as error:
                        raise MarkerCaptureError(
                            "RA4_TLS_RESOURCE:USB_WRITE_ERROR:" + type(error).__name__, text,
                        ) from error
                    echoed_rounds.add(round_number)
                    print(f"TLS_RESOURCE_USB_ECHO_SENT={round_number}")
        if all(marker in text for marker in required):
            if "RA4_TLS_RESOURCE:RESULT=PASS" in required:
                timeline("RESULT_PASS_OBSERVED")
            return text
    text = received.decode("ascii", errors="replace").replace("\x00", "")
    raise MarkerCaptureError(
        "RA4_TLS_RESOURCE:MARKERS_TIMEOUT",
        text,
    )


def require_ra4_boundary_evidence(capture: str, rounds: int, mixed_probe: bool = True) -> None:
    prefix = "RA4_TLS_RESOURCE:RA4_BOUNDARY="
    records = re.findall(r"^" + re.escape(prefix) + r"([^\r\n]*)\r?$", capture, re.MULTILINE)
    stages = (1, 2, 3, 4) if mixed_probe else (1, 2, 4)
    expected = [(r, stage) for r in range(1, rounds + 1) for stage in stages]
    if len(records) != len(expected):
        raise RuntimeError("missing or duplicate RA4 boundary records")
    previous_min, previous_max, heap_limit = 0x20008000, 0, None
    for record, position in zip(records, expected):
        if not re.fullmatch(r"[0-9]+(?:,[0-9]+){6}", record):
            raise RuntimeError("malformed RA4 boundary record")
        round_number, stage, limit, heap, msp, minimum, maximum = map(int, record.split(','))
        if ((round_number, stage) != position or
                any(not 0x20000000 <= v <= 0x20008000
                    for v in (limit, heap, msp, minimum, maximum)) or
                (heap_limit is not None and limit != heap_limit) or
                minimum > min(previous_min, msp) or maximum < max(previous_max, heap)):
            raise RuntimeError("inconsistent RA4 boundary record")
        # Preserve crossings as evidence. Sampled extrema do not measure the
        # deepest call, and historical heap/stack extrema need not coincide.
        previous_min, previous_max, heap_limit = minimum, maximum, limit


def require_ble_ap_evidence(capture: str) -> None:
    restored = "RA4_TLS_RESOURCE:AP_SCAN_RESTORED"
    if capture.count(restored) != 1 or capture.index(restored) > capture.find("RA4_TLS_RESOURCE:BLE_REPORTS_AFTER_AP="):
        raise RuntimeError("missing scan restoration before fresh BLE receive evidence")
    for stage in ("BEFORE", "AFTER"):
        reports = re.findall(rf"RA4_TLS_RESOURCE:BLE_REPORTS_{stage}_AP=(\d+)\r?\n", capture)
        if len(reports) != 1 or int(reports[0]) == 0:
            raise RuntimeError("missing BLE receive evidence across AP restart")


def print_sta_disconnect_diagnostic() -> None:
    try:
        diagnostic = subprocess.run(
            [sys.executable, str(ROOT_DIR / "tools/inspect_bl616cl_gpio35.py"),
             "--sta-disconnect"],
            capture_output=True, text=True, timeout=8, check=False,
        )
        print(diagnostic.stdout.strip())
        if diagnostic.returncode:
            print("STA_DISCONNECT_DIAGNOSTIC_UNAVAILABLE")
    except (OSError, subprocess.SubprocessError):
        print("STA_DISCONNECT_DIAGNOSTIC_UNAVAILABLE")


def print_markers(label: str, text: str) -> None:
    markers = resource_markers(text)
    if markers:
        print(label + "_BEGIN")
        for marker in markers:
            print(marker)
        print(label + "_END")


def resource_markers(text: str) -> list[str]:
    return [
        line.strip()
        for line in text.replace("\x00", "").splitlines()
        if line.strip().startswith("RA4_TLS_RESOURCE:")
    ]


def save_gate_diagnostics(
    error: Exception | None,
    ra4_output: str,
    peer_output: str,
) -> Path:
    result = "failure" if error is not None else "success"
    path = WORK_ROOT / f"{result}-{time.time_ns()}.log"
    lines = [f"RESULT={result.upper()}"]
    if error is not None:
        lines.append(f"ERROR_TYPE={type(error).__name__}")
    if isinstance(error, MarkerCaptureError):
        lines.append(f"MARKER={error.args[0]}")
    lines.extend(resource_markers(ra4_output))
    lines.extend(
        line.strip()
        for line in peer_output.splitlines()
        if line.strip().startswith("TLS_RESOURCE_PEER:")
    )
    path.write_text("\n".join(lines) + "\n", encoding="ascii", errors="replace")
    return path


def compile_sketch(
    args: argparse.Namespace,
    work_dir: Path,
    trusted_ca: Path,
    untrusted_ca: Path,
) -> Path:
    sketch_dir = work_dir / "ra4-wifis3-tls-resource-gate"
    sketch_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(TEMPLATE.with_name("ra4_fault_capture.h"), sketch_dir / "ra4_fault_capture.h")
    shutil.copy2(TEMPLATE.with_name("all_tcp_slots.h"), sketch_dir / "all_tcp_slots.h")
    sketch = sketch_dir / "ra4-wifis3-tls-resource-gate.ino"
    sketch.write_text(
        TEMPLATE.read_text(encoding="ascii")
        .replace("@TLS_RESOURCE_ROUNDS@", str(args.rounds))
        .replace("@RECEIVE_BURST_BYTES@", str(getattr(args, "receive_burst_bytes", 0)))
        .replace("@TCP_RX_BUDGET@", "true" if getattr(args, "tcp_rx_budget", False) else "false")
        .replace("@TLS_WINDOW_TIMING@", "true" if getattr(args, "tls_window_timing", False) else "false")
        .replace("@PRESERVE_STA_DNS@", "true" if getattr(args, "preserve_sta_dns", False) else "false")
        .replace("@STA_IDLE_RECOVERY@", "true" if args.sta_idle_recovery else "false")
        .replace("@STA_QUEUE_RECOVERY@", "true" if args.sta_queue_recovery else "false")
        .replace("@ALL_TCP_SLOTS@", "true" if getattr(args, "all_tcp_slots", False) else "false")
        .replace("@MIXED_PROBE@", "true" if args.mixed_probe else "false")
        .replace("@MIXED_UDP_SEND@", "true" if args.mixed_udp_send else "false")
        .replace("@MIXED_UDP_VARIED_PAYLOAD@", "true" if getattr(args, "mixed_udp_varied_payload", False) else "false")
        .replace("@RA4_HEAP_DIAGNOSTICS@", "true" if getattr(args, "ra4_heap_diagnostics", False) else "false")
        .replace("@RA4_STACK_GUARD@", "true" if getattr(args, "ra4_stack_guard", False) else "false")
        .replace("@SOFTAP@", "true" if getattr(args, "softap", False) else "false")
        .replace("@AP_SCAN_STOP_CONTROL@", "1" if getattr(args, "ap_scan_stop_control", False) else "0")
        .replace("@BLE_BEFORE_AP@", "1" if getattr(args, "ble_before_ap", False) else "0")
        .replace("@BLE_SCAN@", "1" if getattr(args, "ble_scan", False) else "0")
        .replace("@BLE_SCAN_WINDOW@", str(getattr(args, "ble_scan_window", 32)))
        .replace("@BLE_SCAN_INTERVAL@", str(getattr(args, "ble_scan_interval", 32)))
        .replace("@USB_ECHO@", "true" if getattr(args, "usb_echo", False) else "false")
        .replace("@AT_TRACE@", "true" if getattr(args, "at_trace", False) else "false")
        .replace("@INTERRUPT_AFTER_EIGHT_OPEN@",
                 "true" if args.interrupt_after_eight_open else "false")
        .replace(
            "@TLS_TRUSTED_CA_LITERAL@",
            pem_c_literal(trusted_ca.read_text(encoding="ascii")),
        )
        .replace(
            "@TLS_UNTRUSTED_CA_LITERAL@",
            pem_c_literal(untrusted_ca.read_text(encoding="ascii")),
        ),
        encoding="ascii",
    )
    overlay_library = work_dir / "libraries" / "WiFiS3"
    run_command(
        [
            sys.executable,
            str(OVERLAY_PREPARER),
            "--output-library",
            str(overlay_library),
        ] + (["--sized-response-reserve"] if getattr(args, "ra4_sized_response_reserve", False) else [])
    )
    build_dir = work_dir / "build"
    stack_reserve = getattr(args, "ra4_stack_reserve", 1024)
    stack_properties = []
    if stack_reserve != 1024:
        from prepare_wifis3_result_error_overlay import default_source_library
        core = default_source_library().parents[1]
        original = (core / "variants/UNOWIFIR4/fsp.ld").read_text()
        for name in ("__HeapLimit", "__StackLimit"):
            before = name + " = .;"
            if original.count(before) != 1:
                raise RuntimeError("unrecognized RA4 linker stack assignment")
            original = original.replace(before, f"{name} = ABSOLUTE(__StackTop - {stack_reserve});")
        overlay = work_dir / "fsp-stack-budget.ld"
        overlay.write_text(original)
        recipes = [line.split("=", 1)[1] for line in (core / "platform.txt").read_text().splitlines()
                   if line.startswith("recipe.c.combine.pattern=")]
        original_script = '"-T{build.variant.path}/fsp.ld"'
        if len(recipes) != 1 or recipes[0].count(original_script) != 1:
            raise RuntimeError("unrecognized RA4 link recipe")
        # Keep the installed core intact; only allocator/stack limits differ
        # in this temporary script. The physical stack section stays put.
        stack_properties = ["--build-property", "recipe.c.combine.pattern="
                            + recipes[0].replace(original_script, f'"-T{overlay}"')]
    run_command(
        [
            args.arduino_cli,
            "compile",
            "--fqbn",
            FQBN,
            "--build-path",
            str(build_dir),
            "--library",
            str(overlay_library),
            *(["--library", str(ROOT_DIR / "libraries" / "ArduinoBLE")]
              if getattr(args, "ble_scan", False) else []),
            *stack_properties,
            str(sketch_dir),
        ]
    )
    image = build_dir / "ra4-wifis3-tls-resource-gate.ino.bin"
    if not image.is_file():
        raise RuntimeError("compiled RA4M1 TLS resource image does not exist")
    if stack_reserve != 1024:
        validate_ra4_stack_reserve(image.with_suffix(".elf"), stack_reserve)
    return image


def validate_ra4_stack_reserve(elf: Path, reserve: int) -> None:
    compilers = sorted((Path.home() / ".arduino15/packages/arduino/tools/arm-none-eabi-gcc").glob("*/bin/arm-none-eabi-nm"))
    if len(compilers) != 1:
        raise RuntimeError("unique RA4 nm required for stack budget validation")
    output = subprocess.run([str(compilers[0]), "-S", "-C", str(elf)],
                            capture_output=True, text=True, check=True).stdout
    symbols = {name: int(address, 16) for address, name in re.findall(
        r"(?m)^([0-9a-f]+) (?:[0-9a-f]+ )?[A-Za-z] (\S+)$", output)}
    expected = {"__StackTop": 0x20007F00, "__HeapLimit": 0x20007F00 - reserve,
                "__StackLimit": 0x20007F00 - reserve, "g_main_stack": 0x20007B00}
    if any(symbols.get(name) != value for name, value in expected.items()):
        raise RuntimeError("RA4 linker did not preserve MSP/stack object and requested heap boundary: "
                           + str({name: hex(symbols[name]) if name in symbols else None for name in expected}))
    if not 0x20000000 <= symbols.get("__HeapBase", 0) < expected["__HeapLimit"]:
        raise RuntimeError("RA4 stack budget leaves no bounded heap")
    print(f"RA4_STACK_RESERVE_ELF_PASS={reserve},heap_capacity="
          + str(expected["__HeapLimit"] - symbols["__HeapBase"]))


def launch_windows_peer(
    work_dir: Path,
    host: ipaddress.IPv4Address,
    tls_port: int,
    certificate: Path,
    key: Path,
    event_log: Path,
    rounds: int,
    mixed_probe: bool,
    mixed_udp_send: bool = False,
    sta_queue_recovery: bool = False,
    receive_burst_bytes: int = 0,
    mixed_udp_varied_payload: bool = False,
    tcp_diagnostics: bool = False,
    all_tcp_slots: bool = False,
) -> subprocess.Popen[str]:
    script = work_dir / "windows_tls_resource_peer.py"
    script.write_text(textwrap.dedent(WINDOWS_RESOURCE_PEER), encoding="ascii")
    return subprocess.Popen(
        [
            "powershell.exe",
            "-NoProfile",
            "-Command",
            (
                f'& py.exe -3 "{wsl_unc_path(script)}" '
                f"--bind-ip {host} --port {tls_port} "
                f'--cert "{wsl_unc_path(certificate)}" '
                f'--key "{wsl_unc_path(key)}" '
                f'--event-log "{wsl_unc_path(event_log)}" '
                f"--rounds {rounds}"
                + f" --receive-burst-bytes {receive_burst_bytes}"
                + (" --tcp-diagnostics" if tcp_diagnostics else "")
                + (" --all-tcp-slots" if all_tcp_slots else "")
                + (
                    f" --mixed-tcp-port {MIXED_TCP_PORT}"
                    f" --mixed-server-port {MIXED_SERVER_PORT}"
                    if mixed_probe or all_tcp_slots else ""
                )
                + (" --mixed-udp-port 18448" if mixed_udp_send else "")
                + (" --mixed-udp-varied-payload" if mixed_udp_varied_payload else "")
                + (" --sta-queue-recovery" if sta_queue_recovery else "")
            ),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8", errors="replace",
    )


def wait_for_peer_ready(peer: subprocess.Popen[str], timeout_seconds: float) -> str:
    assert peer.stdout is not None
    deadline = time.monotonic() + timeout_seconds
    captured = ""
    while time.monotonic() < deadline:
        if peer.poll() is not None:
            captured += peer.stdout.read()
            break
        line = peer.stdout.readline()
        if line:
            captured += line
            if "TLS_RESOURCE_PEER:READY" in captured:
                return captured
    # Preserve startup exceptions (for example a failed bind) instead of
    # reporting only a generic readiness failure. This peer never receives
    # STA credentials; its output contains markers and generated file paths.
    raise RuntimeError("temporary Windows TLS resource peer did not become ready: " + captured[-4096:].strip())


def stop_resource_peer(peer, work_dir):
    try:
        peer.wait(timeout=1)
        return
    except subprocess.TimeoutExpired:
        pass
    pid = (work_dir / "windows_tls_resource_peer.pid").read_text(encoding="ascii").strip()
    if not pid.isdigit() or int(pid) < 4:
        raise RuntimeError("invalid owned resource peer PID")
    result = subprocess.run(["taskkill.exe", "/PID", pid, "/T", "/F"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
    if result.returncode and peer.poll() is None:
        raise RuntimeError("owned resource peer termination failed")
    peer.wait(timeout=5)
    print("TLS_RESOURCE_PEER_OWNED_TERMINATION_PASS", flush=True)


def reopen_after_association(args, bridge):
    # Recover mapping before closing a stale fd, which can otherwise block in
    # usb_kill_urb. This is preparation only; pressure captures never resume.
    args.bridge_port = ensure_bl_usb_mapping(getattr(args, 'prepared_bl_attach', None))
    if bridge is not None:
        close_serial_bounded(bridge, phase="tls-resource-association")
    reopened = serial.Serial(args.bridge_port, 115200, timeout=0.1, write_timeout=1.0)
    reopened.dtr = False
    reopened.rts = False
    print("TLS_RESOURCE_ASSOCIATION_USB_IDENTITY_REOPEN_PASS", flush=True)
    return reopened


def start_preparation_detach(args):
    # Start and confirm WSL root before compilation/upload/Windows WLAN work.
    # Later startup has intermittently stalled before the Linux entry marker.
    import select
    port = resolve_paired_bl616cl_port(args.bridge_port)
    final_scan.require_final_carrier_bridge(port)
    candidates = stale_bl_ports()
    if len(candidates) != 1:
        raise RuntimeError("unique dedicated BL vhci port unavailable")
    path, number = candidates[0]
    code = r"""
from pathlib import Path
import re,sys
path=Path(sys.argv[1]); number=int(sys.argv[2])
print('BL616CL_PREPARATION_ROOT_READY',flush=True)
if sys.stdin.readline().strip() != 'DETACH': sys.exit(2)
rows=[line.split() for line in (path.parent/'status').read_text().splitlines()[1:]]
matches=[r for r in rows if len(r)==7 and r[0]=='hs' and int(r[1])==number and r[2]=='006']
if len(matches)!=1 or not re.fullmatch(r'\d+-[\d.]+',matches[0][6]): sys.exit(3)
device=Path('/sys/bus/usb/devices')/matches[0][6]
identity=tuple((device/name).read_text().strip() for name in ('idVendor','idProduct','serial'))
if identity!=('2341','1002','BL616CL-PROVISIONAL'): sys.exit(4)
print('BL616CL_PREPARATION_ROOT_DETACH_ENTER',flush=True)
path.write_text(str(number))
print('BL616CL_PREPARATION_ROOT_DETACH_EXIT',flush=True)
"""
    helper = subprocess.Popen(["wsl.exe", "-d", "Ubuntu-22.04", "-u", "root", "--",
                               "python3", "-c", code, str(path), str(number)],
                              stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)
    try:
        if not select.select([helper.stdout], [], [], 10)[0]:
            raise RuntimeError("preparation root helper readiness timeout")
        if helper.stdout.readline().strip() != b"BL616CL_PREPARATION_ROOT_READY":
            raise RuntimeError("preparation root helper did not start")
    except Exception:
        helper.terminate()
        helper.wait(timeout=5)
        raise
    print("BL616CL_PREPARATION_ROOT_READY", flush=True)
    return helper


def suspend_before_association(args, bridge, helper):
    # Close while the old WLAN and its USB/IP transport are still healthy.
    port = resolve_paired_bl616cl_port(args.bridge_port)
    final_scan.require_final_carrier_bridge(port)
    close_serial_bounded(bridge, phase="tls-resource-suspend")
    try:
        output, _ = helper.communicate(input=b"DETACH\n", timeout=10)
    except subprocess.TimeoutExpired as error:
        print((error.stdout or b"").decode("ascii", errors="replace").strip(), flush=True)
        raise
    print(output.decode("ascii", errors="replace").strip(), flush=True)
    if helper.returncode or b"BL616CL_PREPARATION_ROOT_DETACH_EXIT" not in output:
        raise RuntimeError("preparation root helper did not detach dedicated BL")
    print("TLS_RESOURCE_USB_SUSPENDED_BEFORE_ASSOCIATION_PASS", flush=True)


def require_preparation_evidence(capture: str, nonce: str, ble: bool) -> str:
    begin = "RA4_TLS_RESOURCE:PREP_EVIDENCE_BEGIN=" + nonce
    end = "RA4_TLS_RESOURCE:PREP_EVIDENCE_END=" + nonce
    lines = capture.replace("\r", "").splitlines()
    if lines.count(begin) != 1 or lines.count(end) != 1 or lines.index(begin) >= lines.index(end):
        raise RuntimeError("missing or duplicate preparation query response")
    block = "\n".join(lines[lines.index(begin)+1:lines.index(end)]) + "\n"
    if block.count("RA4_TLS_RESOURCE:PREP_EVIDENCE_ORIGIN=CACHED_PREPARATION\n") != 1:
        raise RuntimeError("missing preparation evidence origin")
    records = re.findall(r"(?m)^RA4_TLS_RESOURCE:PREP_STATE=([0-9]+(?:,[0-9]+){7})$", block)
    if len(records) != 1:
        raise RuntimeError("missing unique preparation state")
    values = list(map(int, records[0].split(',')))
    if values[0] != 80 or values[1] != 8 or values[5:] != [2, 0, 1]:
        raise RuntimeError("preparation is not complete")
    if ble:
        require_ble_ap_evidence(block)
        for stage, index in (("BEFORE", 2), ("AFTER", 3)):
            if f"RA4_TLS_RESOURCE:BLE_REPORTS_{stage}_AP={values[index]}\n" not in block:
                raise RuntimeError("preparation BLE counts disagree")
    return block


def query_preparation_evidence(bridge, ble):
    nonce = secrets.token_hex(8)
    payload = f"@PREPSTATUS {nonce}\n".encode("ascii")
    if bridge.write(payload) != len(payload):
        raise RuntimeError("preparation query short write")
    bridge.flush()
    capture = capture_until(bridge, ("RA4_TLS_RESOURCE:PREP_EVIDENCE_END=" + nonce,), 5.0)
    try:
        return require_preparation_evidence(capture, nonce, ble)
    except RuntimeError:
        # Cache queries contain only counters and a generated nonce. Preserve
        # the exact public markers that failed validation, rather than only
        # draining later READY announcements in the outer cleanup handler.
        print_markers("RA4_TLS_RESOURCE_PREPARATION_QUERY_REJECTED", capture)
        raise


def upload_sketch(args: argparse.Namespace, image: Path) -> None:
    # Association preparation may have invalidated an earlier USB/IP mapping.
    # Revalidate before upload; never resume an interrupted pressure capture.
    args.bridge_port = ensure_bl_usb_mapping()
    run_command(
        [
            sys.executable,
            str(SAMBA_RUNNER),
            "--bridge-port",
            args.bridge_port,
            "--image",
            str(image),
            "--runtime-settle-ms",
            str(args.runtime_settle_ms),
            "--attempts",
            "3",
        ]
    )


def restore_scan(args: argparse.Namespace) -> None:
    args.bridge_port = ensure_bl_usb_mapping()
    run_command(
        [
            sys.executable,
            str(final_scan.__file__),
            "--bridge-port",
            args.bridge_port,
            "--arduino-cli",
            args.arduino_cli,
        ]
    )
    print("FINAL_CARRIER_RA4_SCAN_RESTORE_PASS")


AP_JOIN_FIELDS = (
    "active", "attempts", "dhcp_ack_transmitted", "completed", "timeouts",
    "elapsed_ms", "client_ip_reply_acknowledged",
)


def parse_ap_join_diagnostic(captured: str) -> dict[str, int] | None:
    records = re.findall(r"(?m)^RA4_TLS_RESOURCE:AFTER_AP_START_COEX_DIAG=([^\r\n]+)", captured)
    if not records:
        return None
    values = records[-1].split(",")
    if len(values) < 36:  # Older firmware does not expose these counters.
        return None
    return dict(zip(AP_JOIN_FIELDS, (int(value) for value in values[29:36])))


def capture_failure_swd(args, work_dir: Path) -> None:
    """Read dedicated RA4M1 registers before replacing its failed Sketch."""
    args.bridge_port = ensure_bl_usb_mapping()
    from inspect_bl616cl_lwip_allocations import capture as capture_allocations
    try:
        print("TLS_RESOURCE_FAILURE_LWIP_ALLOCATIONS=" + json.dumps(capture_allocations(), sort_keys=True), flush=True)
    except (OSError, RuntimeError) as error:
        print("TLS_RESOURCE_FAILURE_LWIP_ALLOCATIONS_UNAVAILABLE=" + type(error).__name__, flush=True)
    from types import SimpleNamespace
    from run_bl616cl_swd_d6_openocd import resolve_paths, require_final_carrier
    port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier(port)
    descriptor = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    os.close(descriptor)
    openocd, scripts, target = resolve_paths(SimpleNamespace(
        openocd=None, scripts_dir=None, target_config=None))
    diagnostic = WORK_ROOT / f"failure-swd-{time.time_ns()}"
    diagnostic.mkdir()
    fault_reads = ["mdw 0xe000ed28 6"]
    gdb = None
    elf = None
    heap_symbols = {}
    for artifact in (work_dir / "build").glob("*.elf"):
        shutil.copy2(artifact, diagnostic / artifact.name)
        compilers = sorted((Path.home() / ".arduino15/packages/arduino/tools/arm-none-eabi-gcc").glob("*/bin/arm-none-eabi-nm"))
        if len(compilers) == 1:
            gdb = compilers[0].with_name("arm-none-eabi-gdb")
            elf = artifact
            symbols = subprocess.run([str(compilers[0]), "-S", str(artifact)],
                                     capture_output=True, text=True, check=True).stdout
            heap_symbols = {name: int(address, 16) for address, name in re.findall(
                r"(?m)^([0-9a-f]+) (?:[0-9a-f]+ )?[A-Za-z] (\S+)$", symbols)}
            for address, size in re.findall(r"(?m)^([0-9a-f]+) ([0-9a-f]+) [bBdD] (?:regs|ra4_fault_snapshot|ra4_test_phase|ra4_gap_before_poll|ra4_gap_after_poll|ra4_heap_snapshot|ra4_stack_snapshot|ra4_boundary_snapshot|ra4_preparation_snapshot)$", symbols):
                # Arduino's cm_backtrace saved CPU frame and fault registers.
                fault_reads.append(f"mdw 0x{address} {int(size, 16) // 4}")
            # Capture only GAP's fixed control/list prefix, excluding String
            # storage and arbitrary SRAM that could contain test credentials.
            for address in re.findall(r"(?m)^([0-9a-f]+) [0-9a-f]+ [bB] GAPObj$", symbols):
                fault_reads.append(f"mdw 0x{address} 8")
            # Actual program break is needed to distinguish exhausted heap
            # from a stack that crossed the linker's reserved boundary.
            for address in re.findall(r"(?m)^([0-9a-f]+) [0-9a-f]+ [bB] current_heap_end\.\d+$", symbols):
                fault_reads.append(f"mdw 0x{address} 1")
    command = [str(openocd), "-s", str(scripts), "-f", "interface/cmsis-dap.cfg",
               "-c", "cmsis_dap_serial BL616CL-PROVISIONAL", "-f", str(target),
               "-c", "adapter speed 100", "-c", "gdb_port 3335",
               "-c", "init", "-c", "halt"]
    for read in fault_reads + ["reg"]:
        command.extend(["-c", read])
    if gdb is None or elf is None:
        raise RuntimeError("matching RA4M1 ELF/GDB unavailable")
    with (diagnostic / "registers.log").open("w") as output:
        server = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT)
        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                server_log = (diagnostic / "registers.log").read_text()
                if "xPSR:" in server_log and "Listening on port 6666" in server_log:
                    break
                if server.poll() is not None:
                    raise RuntimeError("failure SWD server exited before halt")
                time.sleep(0.1)
            from ra4_nano_heap_diagnostic import TclWordReader, summarize_free_list
            reader = None
            try:
                ends = [value for name, value in heap_symbols.items()
                        if re.fullmatch(r"current_heap_end\.\d+", name)]
                if len(ends) != 1:
                    raise ValueError("unique allocator break symbol unavailable")
                reader = TclWordReader()
                summary = summarize_free_list(reader,
                    reader(heap_symbols["__malloc_free_list"]), heap_symbols["__HeapBase"],
                    reader(ends[0]), heap_symbols["__HeapLimit"])
                (diagnostic / "nano-heap.json").write_text(json.dumps(summary, indent=2) + "\n")
                print("TLS_RESOURCE_RA4_NANO_HEAP=" + json.dumps(summary, sort_keys=True))
            except (OSError, ValueError, KeyError) as error:
                print("TLS_RESOURCE_RA4_NANO_HEAP_UNAVAILABLE=" + str(error))
            finally:
                if reader is not None:
                    reader.close()
            if "ra4_preparation_snapshot" in heap_symbols:
                reader = TclWordReader()
                try:
                    address = heap_symbols["ra4_preparation_snapshot"]
                    preparation = [reader(address + 4 * index) for index in range(8)]
                    (diagnostic / "preparation.json").write_text(json.dumps(preparation) + "\n")
                    print("TLS_RESOURCE_RA4_PREPARATION=" + json.dumps(preparation), flush=True)
                finally:
                    reader.close()
            debugger = [str(gdb), "--batch", str(elf),
                        "-ex", "set print frame-arguments none",
                        "-ex", "target remote localhost:3335",
                        "-ex", "p/x ra4_fault_snapshot",
                        "-ex", "bt 20", "-ex", "monitor resume",
                        "-ex", "monitor shutdown"]
            result = subprocess.run(debugger, capture_output=True, text=True, timeout=20)
            (diagnostic / "backtrace.log").write_text(result.stdout + result.stderr)
            print(result.stdout + result.stderr)
            if result.returncode:
                raise RuntimeError("failure SWD backtrace failed")
            server.wait(timeout=5)
        finally:
            if server.poll() is None:
                server.terminate()
                server.wait(timeout=5)
    print(f"TLS_RESOURCE_FAILURE_SWD_LOG={diagnostic / 'registers.log'}")
    print((diagnostic / "registers.log").read_text())


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tcp-rx-budget", action="store_true",
                        help="Require read-only PCB credit/native heap snapshots at four round boundaries.")
    parser.add_argument("--tls-window-timing", action="store_true",
                        help="Require enabled BL TLS phase counters before/after each receive burst.")
    parser.add_argument("--ra4-sized-response-reserve", action="store_true",
                        help="optional temporary WiFiS3 overlay to avoid small binary-response geometric string growth")
    parser.add_argument("--ra4-heap-diagnostics", action="store_true",
                        help="record RA4 allocator statistics around BLE and mixed TCP accept")
    parser.add_argument("--ra4-stack-guard", action="store_true",
                        help="diagnostic sentinel in the unused bottom 32 bytes of reserved RA4 stack")
    parser.add_argument("--ra4-stack-reserve", type=int, choices=(1024, 2048, 3072, 4096), default=1024,
                        help="diagnostic linker budget; preserve initial MSP and reduce the heap upper limit")
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--runtime-settle-ms", type=int, default=2000)
    parser.add_argument("--ready-timeout-ms", type=int, default=15000)
    parser.add_argument(
        "--gate-timeout-ms", type=int, default=None,
        help="Total runtime window; defaults to at least 90 s and 15 s per round.",
    )
    parser.add_argument(
        "--tls-port",
        type=int,
        default=TLS_PORT,
        help="Windows TLS peer port.",
    )
    parser.add_argument(
        "--rounds",
        type=int,
        default=1,
        help="Repeated eight-slot TLS allocate/fail/close/reuse cycles in one RA4M1 runtime.",
    )
    parser.add_argument("--skip-peer-firewall", action="store_true")
    parser.add_argument("--tcp-diagnostics", action="store_true",
                        help="Read-only Windows per-flow TCP window/retransmit samples during receive bursts")
    parser.add_argument("--all-tcp-slots", action="store_true", help="Hold 8 plain + 8 accepted + 8 TLS; verify 64 bytes per plain stream alongside TLS bursts")
    parser.add_argument("--receive-burst-bytes", type=int, default=0,
                        help="0 disables; 1..16384 bytes from each of eight TLS peers before round-robin draining")
    parser.add_argument("--mixed-udp-varied-payload", action="store_true",
                        help="Validate sequence-dependent varied bytes across the complete mixed UDP datagram")
    parser.add_argument("--preserve-sta-dns", action="store_true",
                        help="Diagnostic direct-IP control: retain the DHCP-provided STA DNS servers.")
    parser.add_argument(
        "--mixed-udp-send", action="store_true",
        help="With --mixed-probe, require actual 16 KiB UDP delivery while eight TLS slots remain active.",
    )
    parser.add_argument(
        "--interrupt-after-eight-open", action="store_true",
        help="Replace the RA4M1 Sketch while eight TLS slots are alive; recovery is tested by the next run.",
    )
    parser.add_argument(
        "--direct-ip",
        action="store_true",
        help="Connect TLS slots by peer IPv4 address instead of hostname DNS.",
    )
    parser.add_argument("--sta-queue-recovery", action="store_true",
                        help="Disconnect/reconnect STA with observed TLS send queues between rounds (at least two rounds).")
    parser.add_argument("--sta-idle-recovery", action="store_true",
                        help="Control for STA recovery: disconnect eight TLS slots without queued writes.")
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--softap", action="store_true",
                        help="Run against Windows joined directly to a generated BL616CL SoftAP; requires --direct-ip.")
    parser.add_argument("--ble-scan", action="store_true",
                        help="Require live BLE reports before and after every TLS resource cycle.")
    parser.add_argument("--softap-first-start", action="store_true",
                        help="With BLE-before-AP, skip bootstrap AP; verify first DHCP lease before pressure.")
    parser.add_argument("--ap-scan-stop-control", action="store_true",
                        help="Diagnostic BLE-before-AP control: stop scanning across AP association, then resume and verify.")
    parser.add_argument("--ble-before-ap", action="store_true",
                        help="With --softap --ble-scan, start BLE scanning before restarting AP and require reports on both sides.")
    parser.add_argument("--ble-scan-window", type=int, default=32,
                        help="Diagnostic scan window in 0.625 ms units; default 32.")
    parser.add_argument("--ble-scan-interval", type=int, default=32,
                        help="Diagnostic scan interval in 0.625 ms units; default 32 preserves continuous scanning.")
    parser.add_argument("--usb-echo", action="store_true",
                        help="Require a USB CDC round trip after every resource cycle.")
    parser.add_argument("--at-trace", action="store_true",
                        help="With --softap, trace only the first TLS allocation/CA/connect per round, after credential commands.")
    parser.add_argument("--failure-swd-snapshot", action="store_true",
                        help="On failure, halt/read/resume dedicated RA4M1 registers before normal scan restoration.")
    parser.add_argument(
        "--mixed-probe",
        action="store_true",
        help="Probe one TCP client and a full UDP send buffer while eight TLS slots remain open.",
    )
    return parser.parse_args()



def resource_gate_budget_ms(rounds: int, receive_burst_bytes: int, explicit_ms=None, all_tcp_slots=False) -> int:
    if explicit_ms is not None:
        return explicit_ms
    # The added burst has its own unchanged 10s read deadline. Account for
    # that stage in the whole-run budget, alongside the existing 15s/cycle.
    return max(90000, rounds * (15000 + (10000 if receive_burst_bytes else 0) + (20000 if all_tcp_slots else 0)))


def main() -> int:
    args = parse_arguments()
    if not 0 <= args.receive_burst_bytes <= 16384:
        raise RuntimeError("receive burst bytes must be 0..16384")
    if args.tcp_rx_budget and not args.all_tcp_slots:
        raise RuntimeError("TCP RX budget audit requires all TCP slots and receive bursts")
    if args.tls_window_timing and not args.receive_burst_bytes:
        raise RuntimeError("TLS window timing requires a receive burst")
    if args.all_tcp_slots and (args.mixed_probe or not args.receive_burst_bytes or args.sta_queue_recovery or args.sta_idle_recovery or args.interrupt_after_eight_open):
        raise RuntimeError("all TCP slots requires receive bursts without mixed probe, STA recovery or interruption")
    if args.preserve_sta_dns and (not args.direct_ip or args.sta_queue_recovery or args.sta_idle_recovery):
        raise RuntimeError("preserve STA DNS requires direct IP without DNS recovery checks")
    if args.sta_idle_recovery:
        args.sta_queue_recovery = True
    if args.softap and (not args.direct_ip or args.sta_queue_recovery or
                        args.interrupt_after_eight_open):
        raise RuntimeError("SoftAP requires --direct-ip without STA recovery or interruption")
    if args.ble_scan and (args.sta_queue_recovery or args.interrupt_after_eight_open):
        raise RuntimeError("BLE resource scan requires uninterrupted Wi-Fi resource cycles")
    if (not 18 <= args.ble_scan_interval <= 4096 or args.ble_scan_interval % 2 or
            not 17 <= args.ble_scan_window <= args.ble_scan_interval or
            (not args.ble_scan and (args.ble_scan_window != 32 or args.ble_scan_interval != 32))):
        raise RuntimeError("scan parameters require BLE and ArduinoBLE-valid interval/window")
    if args.softap_first_start and not (args.softap and args.ble_before_ap):
        raise RuntimeError("first AP start requires --softap --ble-before-ap")
    if args.ap_scan_stop_control and not args.ble_before_ap:
        raise RuntimeError("AP scan-stop control requires --ble-before-ap")
    if args.ble_before_ap and not (args.softap and args.ble_scan):
        raise RuntimeError("BLE-before-AP requires --softap --ble-scan")
    if args.at_trace and not args.softap:
        raise RuntimeError("AT trace requires the generated SoftAP fixture")
    if args.sta_queue_recovery and (args.rounds < 2 or args.interrupt_after_eight_open):
        raise RuntimeError("STA queue recovery requires at least two rounds without interruption")
    args.gate_timeout_ms = resource_gate_budget_ms(
        args.rounds, args.receive_burst_bytes, args.gate_timeout_ms, args.all_tcp_slots)
    print(f"TLS_RESOURCE_GATE_BUDGET_MS={args.gate_timeout_ms}", flush=True)
    if args.mixed_udp_varied_payload and not args.mixed_udp_send:
        raise RuntimeError("varied mixed UDP requires --mixed-udp-send")
    if args.mixed_udp_send and (not args.mixed_probe or args.interrupt_after_eight_open):
        raise RuntimeError("mixed UDP send requires --mixed-probe without interruption injection")
    if (
        args.runtime_settle_ms < 0
        or args.ready_timeout_ms <= 0
        or args.gate_timeout_ms <= 0
        or not 1 <= args.rounds <= 1000
        or args.tls_port <= 0
        or args.tls_port > 65535
    ):
        raise RuntimeError("invalid timeout")

    WORK_ROOT.mkdir(parents=True, exist_ok=True)
    work_dir = Path(tempfile.mkdtemp(prefix="unor4-tls-resource-", dir=WORK_ROOT))
    if args.source_only:
        try:
            trusted_dir = work_dir / "trusted"
            untrusted_dir = work_dir / "untrusted"
            trusted_dir.mkdir()
            untrusted_dir.mkdir()
            trusted_ca, _, _ = create_material(
                trusted_dir,
                "ec",
                ipaddress.IPv4Address("192.0.2.1"),
            )
            untrusted_ca, _, _ = create_material(
                untrusted_dir,
                "ec",
                ipaddress.IPv4Address("192.0.2.1"),
            )
            compile_sketch(args, work_dir, trusted_ca, untrusted_ca)
            print("RA4_WIFIS3_TLS_RESOURCE_SOURCE_COMPILE_PASS")
            return 0
        finally:
            shutil.rmtree(work_dir, ignore_errors=True)

    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    final_scan.require_final_carrier_bridge(args.bridge_port)

    print(f"BL616CL final carrier: {args.bridge_port} (2341:1002 interface 00)")
    print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Privacy: STA credentials and generated PKI material stay out of repository logs.")

    bridge = None
    peer = None
    firewall_rule = None
    mixed_udp_firewall = None
    peer_event_log = work_dir / "windows_tls_resource_peer.events"
    peer_output = ""
    ra4_output = ""
    ra4_modified = False
    ra4_image_installed = False
    primary_error: Exception | None = None
    softap_session = None
    detach_helper = None
    args.prepared_bl_attach = None
    try:
        if args.softap_first_start:
            detach_helper = start_preparation_detach(args)
            from bl616cl_prepared_usb_attach import PreparedBlAttach
            args.prepared_bl_attach = PreparedBlAttach()
        if args.softap:
            from bl616cl_softap_test_session import SoftApTestSession
            ssid = "uno-r4-" + secrets.token_hex(4)
            password = secrets.token_urlsafe(18)
            softap_session = SoftApTestSession(work_dir)
            if not args.softap_first_start:
                ra4_modified = True
                softap_session.start(args, ssid, password)
        else:
            ssid, password = load_private_credentials(args.credential_env)
            if password is None:
                raise RuntimeError("TLS resource gate requires a private WPA password")
        # The dedicated first-start AP DHCP range begins at .100. Verify that
        # exact lease before pressure rather than using a stale upstream IP in PKI.
        host = (ipaddress.IPv4Address("192.168.4.100") if args.softap_first_start
                else ipaddress.IPv4Address(windows_wlan_address()))
        print("TLS_RESOURCE_LINK=" + ("DIRECT_SOFTAP" if args.softap else "UPSTREAM_STA"))
        trusted_dir = work_dir / "trusted"
        untrusted_dir = work_dir / "untrusted"
        trusted_dir.mkdir()
        untrusted_dir.mkdir()
        trusted_ca, certificate, key = create_material(trusted_dir, "ec", host)
        untrusted_ca, _, _ = create_material(untrusted_dir, "ec", host)
        image = compile_sketch(args, work_dir, trusted_ca, untrusted_ca)
        if args.softap:
            ra4_modified = True
            upload_sketch(args, image)
            ra4_image_installed = True
            print("TLS_RESOURCE_SOFTAP_STAGE=RESOURCE_READY")
            bridge = serial.Serial(args.bridge_port, 115200, timeout=0.1,
                                   write_timeout=1.0)
            bridge.dtr = False
            bridge.rts = False
            bridge.reset_input_buffer()
            capture_until(bridge, ("RA4_TLS_RESOURCE:READY",),
                          args.ready_timeout_ms / 1000.0)
            if args.softap_first_start:
                softap_session.prepare_association(args, ssid, password, "192.168.4.1")
            prepare_prefix = "ARMPAP" if args.softap_first_start else "PREPAP"
            prepare = (f"{prepare_prefix} {encode_start_value(ssid)} "
                       f"{encode_start_value(password)} {host} {host} {args.tls_port}\n")
            bridge.write(prepare.encode("ascii"))
            bridge.flush()
            if args.softap_first_start:
                armed = capture_until(bridge, ("RA4_TLS_RESOURCE:SOFTAP_ARMED",), 5.0)
                print_markers("RA4_TLS_RESOURCE_SOFTAP_ARM", armed)
                suspend_before_association(args, bridge, detach_helper)
                bridge = None
                # ARM delays radio startup by 5s plus the original 2s BLE
                # observation. Do not request Windows' one fresh scan before
                # that AP exists and then poll a stale scan cache.
                print("TLS_RESOURCE_SOFTAP_ARM_SETTLE_BEGIN", flush=True)
                time.sleep(10.0)
                softap_session.connect_prepared()
                started = ""
            else:
                print("TLS_RESOURCE_SOFTAP_STAGE=AP_STARTED")
                started = capture_until(bridge, ("RA4_TLS_RESOURCE:SOFTAP_STARTED",), 40.0)
                timeline("AP_STARTED_OBSERVED")
                print_markers("RA4_TLS_RESOURCE_SOFTAP_PREPARE", started)
                softap_session.request_preparation_rejoin()
            bridge = reopen_after_association(args, bridge)
            if "RA4_TLS_RESOURCE:SOFTAP_PEER_READY" in started:
                prepared = started
            else:
                print("TLS_RESOURCE_SOFTAP_STAGE=PEER_READY")
                prepared = capture_until(bridge, ("RA4_TLS_RESOURCE:SOFTAP_PEER_READY",), 40.0)
            print_markers("RA4_TLS_RESOURCE_SOFTAP_PREPARE", prepared)
            join_diagnostic = parse_ap_join_diagnostic(prepared)
            if join_diagnostic is not None:
                print("TLS_RESOURCE_AP_JOIN=" + json.dumps(join_diagnostic, sort_keys=True), flush=True)
            if args.softap_first_start:
                evidence = query_preparation_evidence(bridge, args.ble_before_ap)
                print_markers("RA4_TLS_RESOURCE_PREPARATION_QUERY", evidence)
            elif args.ble_before_ap:
                require_ble_ap_evidence(started if prepared == started else started + prepared)
            # Bind listeners only after the final Sketch reset and DHCP rejoin.
            current_host = ipaddress.IPv4Address(softap_session.current_address())
            if args.softap_first_start and current_host != host:
                raise RuntimeError("first-start DHCP lease differs from generated peer certificate")
            print(f"TLS_RESOURCE_SOFTAP_ADDRESS_CHANGED={int(current_host != host)}")
            host = current_host
        if args.skip_peer_firewall:
            print("TLS_RESOURCE_PEER:FIREWALL_SKIPPED")
        else:
            firewall_rule = configure_peer_firewall(host, args.tls_port)
            if args.mixed_udp_send:
                from run_ra4_wifis3_udp_resource_gate import configure_firewall
                mixed_udp_firewall = configure_firewall(host, 18448)
        peer = launch_windows_peer(
            work_dir,
            host,
            args.tls_port,
            certificate,
            key,
            peer_event_log,
            args.rounds,
            args.mixed_probe,
            args.mixed_udp_send,
            args.sta_queue_recovery,
            args.receive_burst_bytes,
            args.mixed_udp_varied_payload,
            args.tcp_diagnostics,
            args.all_tcp_slots,
        )
        peer_output = wait_for_peer_ready(peer, args.ready_timeout_ms / 1000.0)

        if not args.softap:
            upload_sketch(args, image)
            ra4_image_installed = True
            ra4_modified = True
            bridge = serial.Serial(
                args.bridge_port,
                baudrate=115200,
                timeout=0.1,
                write_timeout=1.0,
            )
            bridge.dtr = False
            bridge.rts = False
            bridge.reset_input_buffer()
            startup = capture_until(
                bridge,
                ("RA4_TLS_RESOURCE:READY",),
                args.ready_timeout_ms / 1000.0,
            )
            print_markers("RA4_TLS_RESOURCE_STARTUP", startup)
        bridge.reset_input_buffer()
        start = (
            ("STARTIP " if args.direct_ip else "START ")
            + encode_start_value(ssid)
            + " "
            + encode_start_value(password)
            + " "
            + (str(host) if args.direct_ip else TLS_HOSTNAME)
            + " "
            + str(host)
            + " "
            + str(args.tls_port)
            + "\n"
        )
        timeline("PRESSURE_START_REQUEST")
        bridge.write(start.encode("ascii"))
        bridge.flush()
        if args.interrupt_after_eight_open:
            ra4_output = capture_until(
                bridge, ("RA4_TLS_RESOURCE:INTERRUPT_READY",),
                args.gate_timeout_ms / 1000.0,
            )
            print_markers("RA4_TLS_RESOURCE_INTERRUPTION", ra4_output)
            peer_output += wait_for_peer_events(
                peer_event_log, ("TLS_RESOURCE_PEER:HANDSHAKE_OK=8",), 10.0,
            )
            print("RA4_TLS_RESOURCE_INTERRUPTION_INJECTED")
            raise InterruptionInjected()
        capture = capture_until(
            bridge,
            (
                "RA4_TLS_RESOURCE:EIGHT_OPEN_PASS",
                "RA4_TLS_RESOURCE:NINTH_EXHAUSTED_PASS",
                "RA4_TLS_RESOURCE:MIDDLE_CLOSE_PASS",
                "RA4_TLS_RESOURCE:FAILED_HANDSHAKE_PASS",
                "RA4_TLS_RESOURCE:FAILED_CLOSE_PASS",
                "RA4_TLS_RESOURCE:REUSE_SOCKET_PASS",
                "RA4_TLS_RESOURCE:REUSE_WRITE_PASS",
                "RA4_TLS_RESOURCE:REUSE_READ_PASS",
                "RA4_TLS_RESOURCE:CLOSE_ALL_PASS",
                "RA4_TLS_RESOURCE:RESULT=PASS",
            ) + (
                (
                    "RA4_TLS_RESOURCE:MIXED_TCP_PASS",
                    "RA4_TLS_RESOURCE:MIXED_UDP_BEGIN_PASS",
                    "RA4_TLS_RESOURCE:MIXED_UDP_PACKET_PASS",
                    "RA4_TLS_RESOURCE:MIXED_UDP_16K_BUFFER_PASS",
                    "RA4_TLS_RESOURCE:MIXED_SERVER_ACCEPT_PASS",
                ) if args.mixed_probe else ()
            ),
            args.gate_timeout_ms / 1000.0,
            idle_timeout_seconds=45.0 if args.sta_queue_recovery else 120.0,
            byte_limit=max(65536, args.rounds * 4096),
            usb_echo=args.usb_echo,
        )
        ra4_output = capture
        if args.tcp_rx_budget:
            from summarize_tcp_rx_budget import summarize as summarize_rx_budget
            try:
                rx_budget = summarize_rx_budget(capture)
            except ValueError as error:
                raise RuntimeError("invalid TCP RX budget evidence") from error
            if len(rx_budget) != args.rounds * 4:
                raise RuntimeError("missing TCP RX budget rounds")
            for row in rx_budget:
                if row["label"] in ("BEFORE_RX", "AFTER_RX") and row["established"] != 24:
                    raise RuntimeError("TCP RX budget audit did not observe all 24 established flows")
            print("TCP_RX_BUDGET_SUMMARY=" + json.dumps(rx_budget), flush=True)
        if args.tls_window_timing:
            from summarize_tls_window_timing import summarize
            try:
                timing = summarize(capture)
            except ValueError as error:
                raise RuntimeError("invalid TLS phase timing evidence") from error
            if len(timing) != args.rounds * 3:
                raise RuntimeError("missing TLS phase timing rounds")
            print("TLS_WINDOW_TIMING_SUMMARY=" + json.dumps(timing), flush=True)
        if args.receive_burst_bytes:
            for round_number in range(1, args.rounds + 1):
                if f"RA4_TLS_RESOURCE:RX_BURST_PASS={round_number},{args.receive_burst_bytes}\n" not in capture.replace("\r", ""):
                    raise RuntimeError("missing eight-slot receive burst validation")
        if args.ra4_heap_diagnostics:
            require_ra4_boundary_evidence(capture, args.rounds, args.mixed_probe)
        if args.ble_scan:
            for stage in ("BEFORE", "AFTER"):
                reports = re.findall(rf"RA4_TLS_RESOURCE:BLE_REPORTS_{stage}=(\d+),(\d+)\r?\n", capture)
                if ([int(round_number) for round_number, _ in reports] !=
                        list(range(1, args.rounds + 1)) or
                        any(int(count) == 0 for _, count in reports)):
                    raise RuntimeError("missing per-round BLE receive recovery evidence")
        if args.usb_echo:
            observed = re.findall(r"RA4_TLS_RESOURCE:USB_ECHO_PASS=(\d+)\r?\n", capture)
            if observed != [str(i) for i in range(1, args.rounds + 1)]:
                raise RuntimeError("missing per-round USB CDC round trips")
        if args.mixed_udp_send:
            for sequence in range(1, args.rounds + 1):
                if f"RA4_TLS_RESOURCE:MIXED_UDP_16K_SEND_PASS={sequence}\n" not in capture.replace("\r", ""):
                    raise RuntimeError("missing mixed UDP delivery acknowledgement")
        if args.sta_queue_recovery:
            for cycle in range(1, args.rounds):
                for stage in (("STA_IDLE_CONTROL" if args.sta_idle_recovery else "STA_QUEUE_OBSERVED"), "STA_DISCONNECTED", "STA_DNS_RESTORED", "STA_RECONNECTED"):
                    if f"RA4_TLS_RESOURCE:{stage}={cycle}\n" not in capture.replace("\r", ""):
                        raise RuntimeError("missing STA queue recovery evidence")
        print_markers("RA4_TLS_RESOURCE", capture)
        required_peer_events = [
            "TLS_RESOURCE_PEER:HANDSHAKE_OK=1",
            "TLS_RESOURCE_PEER:HANDSHAKE_OK=2",
            "TLS_RESOURCE_PEER:HANDSHAKE_OK=3",
            "TLS_RESOURCE_PEER:HANDSHAKE_OK=4",
            "TLS_RESOURCE_PEER:HANDSHAKE_OK=5",
            "TLS_RESOURCE_PEER:HANDSHAKE_OK=6",
            "TLS_RESOURCE_PEER:HANDSHAKE_OK=7",
            "TLS_RESOURCE_PEER:HANDSHAKE_OK=8",
            "TLS_RESOURCE_PEER:HANDSHAKE_REJECTED=9",
            "TLS_RESOURCE_PEER:HANDSHAKE_OK=10",
            "TLS_RESOURCE_PEER:HANDSHAKE_REJECTED="
            + str(args.rounds * 10 - 1),
            "TLS_RESOURCE_PEER:HANDSHAKE_OK="
            + str(args.rounds * 10),
            "TLS_RESOURCE_PEER:REUSE_EXCHANGE_PASS="
            + str(args.rounds),
        ]
        if args.receive_burst_bytes:
            required_peer_events.extend(
                f"TLS_RESOURCE_PEER:RX_BURST_SENT={cycle * 10 + slot + 1},{args.receive_burst_bytes}\n"
                for cycle in range(args.rounds) for slot in range(8)
            )
        if args.sta_queue_recovery and not args.sta_idle_recovery:
            required_peer_events.extend(
                f"TLS_RESOURCE_PEER:QUEUE_HOLD={cycle * 10 + 1}\n"
                for cycle in range(args.rounds - 1)
            )
        if not args.direct_ip:
            required_peer_events.insert(0, "TLS_RESOURCE_PEER:DNS_QUERY")
        if args.all_tcp_slots:
            for cycle in range(1, args.rounds + 1):
                if f"RA4_TLS_RESOURCE:ALL_TCP_CLOSE_PASS={cycle}\n" not in capture.replace("\r", ""):
                    raise RuntimeError("missing 24 TCP cleanup evidence")
                for marker, suffix in (("ALL_TCP_OPEN_PASS", "8,8,8"), ("ALL_TCP_CONTENT_PASS", "16,64")):
                    if f"RA4_TLS_RESOURCE:{marker}={cycle},{suffix}\n" not in capture.replace("\r", ""):
                        raise RuntimeError("missing 24 TCP capacity/content evidence")
            required_peer_events.extend(f"TLS_RESOURCE_PEER:ALL_TCP_{kind}_CONTENT_SENT={index},64\n"
                for kind in ("PLAIN", "ACCEPTED") for index in range(1, args.rounds*8 + 1))
        if args.mixed_probe:
            required_peer_events.append("TLS_RESOURCE_PEER:MIXED_TCP_ACCEPTED")
            required_peer_events.append("TLS_RESOURCE_PEER:MIXED_SERVER_CONNECTED")
        if args.mixed_udp_send:
            required_peer_events.extend(
                f"TLS_RESOURCE_PEER:MIXED_UDP_16K_RECEIVED={sequence}\n"
                for sequence in range(1, args.rounds + 1)
            )
        peer_output += wait_for_peer_events(
            peer_event_log,
            tuple(required_peer_events),
            10.0,
        )
        if args.ap_scan_stop_control:
            print("RA4_WIFIS3_TLS_RESOURCE_AP_SCAN_STOP_CONTROL_PASS")
        else:
            print("RA4_WIFIS3_TLS_RESOURCE_GATE_PASS")
    except InterruptionInjected:
        pass
    except MarkerCaptureError as error:
        ra4_output = error.captured
        print_markers("RA4_TLS_RESOURCE_DIAGNOSTICS", error.captured)
        primary_error = error
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
        if args.softap and not ra4_output and bridge is not None and bridge.is_open:
            # Preparation can fail in a host helper while the Sketch reports
            # its own AP/scan failure. Preserve that pending serial evidence.
            try:
                pending = bytearray()
                deadline = time.monotonic() + 1.0
                while time.monotonic() < deadline and len(pending) < 16384:
                    pending.extend(bridge.read(min(4096, 16384 - len(pending))))
                ra4_output = pending.decode("ascii", errors="replace")
                print_markers("RA4_TLS_RESOURCE_PREPARATION_FAILURE", ra4_output)
            except (OSError, serial.SerialException):
                print("TLS_RESOURCE_PREPARATION_SERIAL_UNAVAILABLE")
    finally:
        timeline("CLEANUP_BEGIN")
        if getattr(args, 'prepared_bl_attach', None) is not None:
            try:
                args.prepared_bl_attach.close()
            except subprocess.SubprocessError as error:
                print("TLS_RESOURCE_ATTACH_HELPER_CLEANUP_FAILED", file=sys.stderr)
                if primary_error is None:
                    primary_error = error
        if detach_helper is not None and detach_helper.poll() is None:
            detach_helper.terminate()
            try:
                detach_helper.wait(timeout=5)
            except subprocess.SubprocessError as error:
                print("TLS_RESOURCE_DETACH_HELPER_CLEANUP_FAILED", file=sys.stderr)
                if primary_error is None:
                    primary_error = error
        if bridge is not None and bridge.is_open:
            try:
                close_serial_bounded(bridge, phase="tls-resource-cleanup")
            except RuntimeError as error:
                if primary_error is None:
                    primary_error = error
            finally:
                bridge = None
        if primary_error is not None and args.failure_swd_snapshot and ra4_image_installed:
            try:
                capture_failure_swd(args, work_dir)
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                print(f"TLS_RESOURCE_FAILURE_SWD_CAPTURE_FAILED={error}", file=sys.stderr)
        if args.sta_queue_recovery and ra4_modified:
            # Capture teardown state before scan restoration starts another
            # disconnect. A wedged USB path must not hide the primary failure.
            print_sta_disconnect_diagnostic()
        if softap_session is not None:
            try:
                softap_session.close()
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                if primary_error is None:
                    primary_error = error
                else:
                    print("TLS_RESOURCE_SOFTAP_CLEANUP_FAILED", file=sys.stderr)
        # Release the WLAN helper before ending this run's owned peer process.
        if peer is not None:
            try:
                stop_resource_peer(peer, work_dir)
            except (OSError, RuntimeError, subprocess.SubprocessError) as error:
                print("TLS_RESOURCE_PEER_CLEANUP_FAILED", file=sys.stderr)
                if primary_error is None:
                    primary_error = error
        remove_peer_firewall(firewall_rule)
        if mixed_udp_firewall is not None:
            from run_ra4_wifis3_udp_resource_gate import remove_firewall
            remove_firewall(mixed_udp_firewall)
        peer_output = read_peer_event_log(peer_event_log) or peer_output
        for line in peer_output.splitlines():
            if line.startswith("TLS_RESOURCE_PEER:"):
                print(line)
        if ra4_modified:
            try:
                restore_scan(args)
            except (
                OSError,
                RuntimeError,
                serial.SerialException,
                subprocess.SubprocessError,
            ):
                print("FINAL_CARRIER_RA4_SCAN_RESTORE_FAILED", file=sys.stderr)
                if primary_error is None:
                    primary_error = RuntimeError(
                        "final-carrier RA4M1 scan restore failed"
                    )
        if args.at_trace:
            trace = [line for line in ra4_output.splitlines()
                     if line.strip().startswith(("REQUEST:", "RAW:", "<-RAW END", "ANSWER:", "Result:"))]
            for line in trace:
                print(line)
        path = save_gate_diagnostics(primary_error, ra4_output, peer_output)
        if args.at_trace:
            with path.open("a", encoding="ascii", errors="replace") as output:
                output.write("\n".join(trace) + "\n")
        print(f"TLS_RESOURCE_GATE_LOG={path}")
        shutil.rmtree(work_dir, ignore_errors=True)

    if primary_error is not None:
        raise primary_error
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
