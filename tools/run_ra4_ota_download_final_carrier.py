#!/usr/bin/env python3
"""Validate BL616CL RA4 OTA HTTP(S) download/cache verification on the final carrier.

The gate uploads the temporary RA4M1 Serial <-> Serial2 AT proxy, joins the
private STA test WLAN, and streams a generated valid UNO R4 OTA container from
a temporary Windows HTTP or TLS peer. It proves response framing, async
progress, LZSS, CRC, and LittleFS cache commit through the real carrier. HTTPS
also proves the raw CA upload and verified TLS handshake path. It does not send
+OTAUPDATE and never writes RA4M1 Flash as part of the OTA test itself.
"""

from __future__ import annotations

import argparse
import binascii
import ipaddress
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import threading
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_bl616cl_apsta_gate import (
    DEFAULT_CREDENTIAL_ENV,
    load_private_credentials,
    quote_at_argument,
)
import run_ra4_wifis3_scan_final_carrier as final_scan
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT_DIR = Path(__file__).resolve().parents[1]
FQBN = "arduino:renesas_uno:unor4wifi"
PROXY_SKETCH = ROOT_DIR / "examples" / "ra4-at-transcript-proxy"
PROXY_BUILD = ROOT_DIR / "build" / "ra4-ota-download-proxy-final-carrier"
PROXY_IMAGE = PROXY_BUILD / "ra4-at-transcript-proxy.ino.bin"
SAMBA_RUNNER = ROOT_DIR / "tools" / "run_ra4_bossac_final_carrier.py"
WORK_ROOT = ROOT_DIR / "build" / "ra4-ota-download-final-carrier"
OTA_PORT = 53
OTA_MAGIC = 0x23411002
OTA_PATH = "/ra4-ota-http-gate.bin"
STATUS_PATTERN = re.compile(rb"^\+GETSTATUS: ([0-9]{1,3})\r\nOK\r\n$")
PROGRESS_PATTERN = re.compile(rb"^\+OTADOWNLOADPROGRESS: ([0-9]+)\r\nOK\r\n$")


WINDOWS_OTA_PEER = r"""
import argparse
import ctypes
import pathlib
import socket
import ssl
import sys
import threading
import time


def monitor_tcp(connection, stop, marker):
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
            print(marker + ":TCP_INFO_UNAVAILABLE="
                  + str(winsock.WSAGetLastError()), flush=True)
            return
        print(marker + ":TCP_INFO=" + ",".join(
            name + ":" + str(getattr(info, name)) for name in (
                "State", "Mss", "BytesInFlight", "Cwnd", "SndWnd",
                "BytesOut", "BytesRetrans", "DupAcksIn", "TimeoutEpisodes")),
              flush=True)
        stop.wait(5)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind-ip", required=True)
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--package", required=True)
    parser.add_argument("--chunk-bytes", type=int, required=True)
    parser.add_argument("--chunk-delay-ms", type=int, required=True)
    parser.add_argument("--tcp-diagnostics", action="store_true")
    parser.add_argument("--tcp-nodelay", action="store_true")
    parser.add_argument("--https", action="store_true")
    parser.add_argument("--cert")
    parser.add_argument("--key")
    args = parser.parse_args()

    package = pathlib.Path(args.package).read_bytes()
    if not package or args.chunk_bytes <= 0 or args.chunk_delay_ms < 0:
        raise RuntimeError("invalid HTTP peer input")
    if args.https and (not args.cert or not args.key):
        raise RuntimeError("HTTPS peer requires a certificate and key")

    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.bind_ip, args.port))
    listener.listen(1)
    listener.settimeout(30)
    marker = "HTTPS_OTA_PEER" if args.https else "HTTP_OTA_PEER"
    context = None
    if args.https:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(certfile=args.cert, keyfile=args.key)
    print(marker + ":READY", flush=True)
    monitor_stop = threading.Event()
    monitor = None
    try:
        connection, _ = listener.accept()
        with connection:
            if args.tcp_nodelay:
                connection.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
                print(marker + ":TCP_NODELAY=1", flush=True)
            transport = connection
            if context is not None:
                try:
                    transport = context.wrap_socket(connection, server_side=True)
                    print(marker + ":HANDSHAKE_PASS", flush=True)
                except ssl.SSLError as error:
                    reason = getattr(error, "reason", None) or "unknown"
                    print(marker + ":HANDSHAKE_ERROR=" + reason, flush=True)
                    raise
            with transport:
                if args.tcp_diagnostics:
                    monitor = threading.Thread(target=monitor_tcp,
                                               args=(transport, monitor_stop, marker), daemon=True)
                    monitor.start()
                request = bytearray()
                while b"\r\n\r\n" not in request:
                    chunk = transport.recv(512)
                    if not chunk:
                        raise RuntimeError("HTTP client closed before request")
                    request.extend(chunk)
                    if len(request) > 4096:
                        raise RuntimeError("HTTP request too large")
                if not request.startswith(b"GET /ra4.ota HTTP/1.1\r\n"):
                    raise RuntimeError("unexpected HTTP request path")
                print(marker + ":REQUEST", flush=True)
                header = (
                    b"HTTP/1.1 200 OK\r\n"
                    + b"Content-Length: " + str(len(package)).encode("ascii") + b"\r\n"
                    + b"Content-Type: application/octet-stream\r\n"
                    + b"Connection: close\r\n\r\n"
                )
                transport.sendall(header)
                started = time.monotonic()
                sent = 0
                next_progress = 8192
                for offset in range(0, len(package), args.chunk_bytes):
                    chunk = package[offset : offset + args.chunk_bytes]
                    transport.sendall(chunk)
                    sent += len(chunk)
                    if sent >= next_progress or sent == len(package):
                        # sendall completion means accepted by the local TCP
                        # stack; it does not prove delivery to the board.
                        print(marker + ":SEND_PROGRESS=" + str(sent)
                              + "," + str(int((time.monotonic() - started) * 1000)),
                              flush=True)
                        next_progress = sent + 8192
                    if offset + args.chunk_bytes < len(package):
                        time.sleep(args.chunk_delay_ms / 1000.0)
                print(marker + ":PAYLOAD_SENT", flush=True)
                if args.tcp_diagnostics and not args.https:
                    # Send FIN at the same point as the original close, but
                    # retain the socket for TCP telemetry until the board
                    # closes its side. This also observes stalls after all
                    # bytes have entered the Windows send buffer.
                    transport.shutdown(socket.SHUT_WR)
                    transport.settimeout(35)
                    try:
                        while transport.recv(512):
                            pass
                        print(marker + ":CLIENT_CLOSED", flush=True)
                    except socket.timeout:
                        print(marker + ":CLIENT_CLOSE_TIMEOUT", flush=True)
    finally:
        monitor_stop.set()
        if monitor is not None:
            monitor.join(timeout=1)
        listener.close()


if __name__ == "__main__":
    main()
"""


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def wsl_unc_path(path: Path) -> str:
    return "\\\\wsl.localhost\\Ubuntu-22.04" + str(path.resolve())


def windows_wlan_address() -> ipaddress.IPv4Address:
    command = (
        '$ErrorActionPreference = "Stop"; '
        'Get-NetIPConfiguration | '
        'Where-Object { $_.InterfaceAlias -eq "WLAN" -and $_.IPv4Address -and $_.IPv4DefaultGateway } | '
        'ForEach-Object { [PSCustomObject]@{ '
        'Address=$_.IPv4Address.IPAddress; PrefixLength=$_.IPv4Address.PrefixLength } } | '
        'ConvertTo-Json -Compress'
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", command],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError("cannot query the Windows WLAN IPv4 configuration")
    try:
        data = json.loads(result.stdout)
        if isinstance(data, list):
            if len(data) != 1:
                raise ValueError()
            data = data[0]
        address = ipaddress.IPv4Address(data["Address"])
        network = ipaddress.IPv4Network(
            f"{address}/{int(data['PrefixLength'])}",
            strict=False,
        )
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise RuntimeError("Windows WLAN does not have one usable IPv4 route") from error
    if network.prefixlen != 24:
        raise RuntimeError("OTA HTTP gate currently requires a /24 Windows WLAN IPv4 network")
    return address


def create_tls_material(
    work_dir: Path,
    host: ipaddress.IPv4Address,
) -> tuple[Path, Path]:
    certificate = work_dir / "ca.pem"
    key = work_dir / "server.key"
    csr = work_dir / "server.csr"
    config = work_dir / "openssl.cnf"
    database = work_dir / "index.txt"
    serial_number = work_dir / "serial"
    new_certs = work_dir / "newcerts"
    new_certs.mkdir()
    database.touch()
    serial_number.write_text("01\n", encoding="ascii")
    config.write_text(
        "\n".join(
            [
                "[ ca ]",
                "default_ca = CA_default",
                "[ CA_default ]",
                f"database = {database}",
                f"serial = {serial_number}",
                f"new_certs_dir = {new_certs}",
                "default_md = sha256",
                "default_days = 1",
                "email_in_dn = no",
                "rand_serial = no",
                "policy = policy_any",
                "x509_extensions = v3_req",
                "[ policy_any ]",
                "commonName = supplied",
                "[ v3_req ]",
                "basicConstraints = critical,CA:TRUE",
                "keyUsage = critical,digitalSignature,keyCertSign",
                "extendedKeyUsage = serverAuth",
                f"subjectAltName = IP:{host}",
                "",
            ]
        ),
        encoding="ascii",
    )
    run_command(
        [
            "openssl",
            "req",
            "-newkey",
            "ec",
            "-pkeyopt",
            "ec_paramgen_curve:prime256v1",
            "-nodes",
            "-subj",
            f"/CN={host}",
            "-keyout",
            str(key),
            "-out",
            str(csr),
        ]
    )
    run_command(
        [
            "openssl",
            "ca",
            "-batch",
            "-selfsign",
            "-notext",
            "-config",
            str(config),
            "-keyfile",
            str(key),
            "-in",
            str(csr),
            "-out",
            str(certificate),
            "-startdate",
            "19700101000000Z",
            "-enddate",
            "20400101000000Z",
        ]
    )
    return certificate, key


def make_literal_ota(decoded_size: int) -> bytes:
    if decoded_size <= 0:
        raise RuntimeError("OTA decoded fixture size must be positive")

    decoded = bytes((index * 29 + 17) & 0xFF for index in range(decoded_size))
    compressed = bytearray()
    bits = 0
    bit_count = 0
    for value in decoded:
        bits = (bits << 9) | 0x100 | value
        bit_count += 9
        while bit_count >= 8:
            bit_count -= 8
            compressed.append((bits >> bit_count) & 0xFF)
            bits &= (1 << bit_count) - 1
    if bit_count != 0:
        compressed.append((bits << (8 - bit_count)) & 0xFF)

    header = bytearray(20)
    container_length = 12 + len(compressed)
    header[0:4] = container_length.to_bytes(4, "little")
    header[8:12] = OTA_MAGIC.to_bytes(4, "little")
    crc = binascii.crc32(header[8:20] + compressed) & 0xFFFFFFFF
    header[4:8] = crc.to_bytes(4, "little")
    return bytes(header + compressed)


def compile_proxy(args: argparse.Namespace) -> Path:
    run_command(
        [
            args.arduino_cli,
            "compile",
            "--fqbn",
            FQBN,
            "--build-path",
            str(PROXY_BUILD),
            str(PROXY_SKETCH),
        ]
    )
    if not PROXY_IMAGE.is_file():
        raise RuntimeError("compiled RA4M1 AT proxy image does not exist")
    return PROXY_IMAGE


def upload_proxy(args: argparse.Namespace, image: Path) -> None:
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
        ]
    )


def open_bridge(port: str) -> serial.Serial:
    link = serial.Serial(
        port,
        baudrate=115200,
        timeout=0.1,
        write_timeout=1.0,
    )
    link.dtr = False
    link.rts = False
    return link


def read_until_terminal(
    link: serial.Serial,
    timeout_seconds: float,
    maximum_bytes: int = 512,
) -> bytes:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        chunk = link.read(min(256, maximum_bytes - len(received)))
        if not chunk:
            continue
        received.extend(chunk)
        if received.endswith((b"OK\r\n", b"ERROR\r\n")):
            return bytes(received)
        if len(received) >= maximum_bytes:
            raise RuntimeError("AT response exceeded its byte limit")
    raise RuntimeError(f"AT response timed out with {len(received)} byte(s)")


def transact(link: serial.Serial, request: bytes, timeout_seconds: float) -> bytes:
    link.reset_input_buffer()
    if link.write(request) != len(request):
        raise RuntimeError("AT request write was incomplete")
    link.flush()
    return read_until_terminal(link, timeout_seconds)


def capture_ota_download_telemetry(tcp_diagnostics: bool = False) -> str:
    reports: list[str] = []
    for option in ("--ra4-ota-download", "--ra4-ota-download-backend"):
        result = subprocess.run(
            [
                sys.executable,
                str(ROOT_DIR / "tools" / "inspect_bl616cl_gpio35.py"),
                option,
            ],
            cwd=ROOT_DIR,
            text=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=10,
            check=False,
        )
        output = result.stdout.strip()
        if result.returncode != 0:
            output = (
                f"RA4_OTA_TELEMETRY_CAPTURE_FAILED option={option} "
                f"exit={result.returncode} output={output[:256]}"
            )
        reports.append(output)
    if tcp_diagnostics:
        result = subprocess.run(
            [sys.executable, str(ROOT_DIR / "tools/inspect_bl616cl_sta_tcp.py")],
            cwd=ROOT_DIR, text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            timeout=10, check=False,
        )
        reports.append(result.stdout.strip() if result.returncode == 0 else
                       f"STA_TCP_DIAGNOSTIC_UNAVAILABLE exit={result.returncode}")
    return "\n".join(report for report in reports if report)


def transact_synchronous_download_with_telemetry(
    link: serial.Serial,
    request: bytes,
    timeout_seconds: float,
    tcp_diagnostics: bool = False,
) -> bytes:
    response: list[bytes] = []
    failure: list[BaseException] = []

    def worker() -> None:
        try:
            response.append(transact(link, request, timeout_seconds))
        except BaseException as error:
            failure.append(error)

    thread = threading.Thread(target=worker, name="ra4-ota-sync", daemon=True)
    deadline = time.monotonic() + timeout_seconds + 1.0
    thread.start()
    thread.join(timeout=min(0.75, timeout_seconds))
    if thread.is_alive():
        print("RA4_OTA_SYNC_TELEMETRY_DURING_DOWNLOAD", flush=True)
        telemetry = capture_ota_download_telemetry(tcp_diagnostics)
        if telemetry:
            print(telemetry, flush=True)
    while thread.is_alive():
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        thread.join(timeout=min(5.0, remaining))
        if thread.is_alive() and time.monotonic() < deadline:
            print("RA4_OTA_SYNC_TELEMETRY_PROGRESS", flush=True)
            telemetry = capture_ota_download_telemetry(tcp_diagnostics)
            if telemetry:
                print(telemetry, flush=True)
    if thread.is_alive():
        raise RuntimeError("synchronous OTA transaction did not finish")
    print("RA4_OTA_SYNC_TELEMETRY_FINAL")
    telemetry = capture_ota_download_telemetry(tcp_diagnostics)
    if telemetry:
        print(telemetry)
    if failure:
        raise failure[0]
    if len(response) != 1:
        raise RuntimeError("synchronous OTA transaction produced no response")
    return response[0]


def transact_raw_payload(
    link: serial.Serial,
    header: bytes,
    payload: bytes,
    packet_bytes: int,
    interpacket_delay_seconds: float,
    timeout_seconds: float,
) -> bytes:
    if not header.endswith(b"\r\n") or not payload or packet_bytes <= 0:
        raise RuntimeError("invalid raw AT payload transaction")
    link.reset_input_buffer()
    if link.write(header) != len(header):
        raise RuntimeError("AT raw payload header write was incomplete")
    link.flush()
    for offset in range(0, len(payload), packet_bytes):
        packet = payload[offset : offset + packet_bytes]
        if link.write(packet) != len(packet):
            raise RuntimeError("AT raw payload write was incomplete")
        link.flush()
        if offset + packet_bytes < len(payload):
            time.sleep(interpacket_delay_seconds)
    return read_until_terminal(link, timeout_seconds)


def require_exact(response: bytes, expected: bytes, label: str) -> None:
    if response != expected:
        limit = 256
        suffix = "..." if len(response) > limit else ""
        raise RuntimeError(
            f"{label} did not match the expected AT framing: "
            f"received={response[:limit]!r}{suffix}"
        )


def parse_status(response: bytes) -> int:
    match = STATUS_PATTERN.fullmatch(response)
    if match is None:
        raise RuntimeError("GETSTATUS did not match the expected AT framing")
    return int(match.group(1))


def wait_for_sta(
    link: serial.Serial,
    timeout_seconds: float,
    response_timeout_seconds: float,
) -> list[int]:
    deadline = time.monotonic() + timeout_seconds
    statuses: list[int] = []
    while time.monotonic() < deadline:
        status = parse_status(
            transact(link, b"AT+GETSTATUS?\r\n", response_timeout_seconds)
        )
        if not statuses or statuses[-1] != status:
            statuses.append(status)
        if status == 3:
            return statuses
        time.sleep(0.25)
    raise RuntimeError("BL616CL STA did not reach GETSTATUS=3")


def launch_windows_peer(
    work_dir: Path,
    host: ipaddress.IPv4Address,
    package: Path,
    chunk_bytes: int,
    chunk_delay_ms: int,
    https: bool,
    certificate: Path | None = None,
    key: Path | None = None,
    tcp_diagnostics: bool = False,
    tcp_nodelay: bool = False,
) -> subprocess.Popen[str]:
    if https and (certificate is None or key is None):
        raise RuntimeError("HTTPS peer material is missing")
    script = work_dir / "windows_ota_peer.py"
    script.write_text(textwrap.dedent(WINDOWS_OTA_PEER), encoding="ascii")
    tls_options = ""
    if https:
        tls_options = (
            f' --https --cert "{wsl_unc_path(certificate)}"'
            f' --key "{wsl_unc_path(key)}"'
        )
    return subprocess.Popen(
        [
            "powershell.exe",
            "-NoProfile",
            "-Command",
            (
                f'& py.exe -3 "{wsl_unc_path(script)}" '
                f"--bind-ip {host} --port {OTA_PORT} "
                f'--package "{wsl_unc_path(package)}" '
                f"--chunk-bytes {chunk_bytes} --chunk-delay-ms {chunk_delay_ms}"
                + (" --tcp-diagnostics" if tcp_diagnostics else "")
                + (" --tcp-nodelay" if tcp_nodelay else "")
                + tls_options
            ),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def wait_for_peer_ready(
    peer: subprocess.Popen[str],
    marker: str,
    timeout_seconds: float,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    captured = ""
    assert peer.stdout is not None
    while time.monotonic() < deadline:
        if peer.poll() is not None:
            captured += peer.stdout.read()
            break
        line = peer.stdout.readline()
        if line:
            captured += line
            if f"{marker}:READY" in captured:
                return captured
    raise RuntimeError("temporary Windows OTA peer did not become ready")


def stop_windows_peer(peer: subprocess.Popen[str], work_dir: Path) -> None:
    # A WSL process PID is not a Windows PID. Identify only this run's peer
    # by its unique script directory, using the existing Windows-aware helper.
    from run_ra4_wifis3_tls_gate import stop_windows_peer as stop_scoped_peer
    stop_scoped_peer(peer, work_dir)


def restore_scan(args: argparse.Namespace) -> None:
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


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--response-timeout-ms", type=int, default=2000)
    parser.add_argument("--sta-timeout-ms", type=int, default=45000)
    parser.add_argument("--download-timeout-ms", type=int, default=45000)
    parser.add_argument("--peer-ready-timeout-ms", type=int, default=10000)
    parser.add_argument("--runtime-settle-ms", type=int, default=1000)
    parser.add_argument("--decoded-fixture-bytes", type=int, default=98304)
    parser.add_argument("--peer-chunk-bytes", type=int, default=512)
    parser.add_argument("--peer-chunk-delay-ms", type=int, default=15)
    parser.add_argument("--tcp-diagnostics", action="store_true",
                        help="Read Windows TCP counters and retain plain peer read-side until client close.")
    parser.add_argument("--peer-tcp-nodelay", action="store_true",
                        help="Diagnostic: disable peer Nagle coalescing with unchanged chunk size and pacing.")
    parser.add_argument("--raw-payload-packet-bytes", type=int, default=32)
    parser.add_argument("--raw-payload-interpacket-delay-ms", type=int, default=10)
    parser.add_argument(
        "--synchronous",
        action="store_true",
        help="Validate the blocking +OTADOWNLOAD command instead of the async command pair.",
    )
    parser.add_argument(
        "--https",
        action="store_true",
        help="Use a temporary certificate-authenticated Windows HTTPS peer.",
    )
    parser.add_argument(
        "--source-only",
        action="store_true",
        help="Generate the OTA fixture, optional TLS material, and proxy build without hardware access.",
    )
    parser.add_argument("--softap", action="store_true", help="Use the isolated board AP and restore the original host WLAN.")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.response_timeout_ms <= 0
        or args.sta_timeout_ms <= 0
        or args.download_timeout_ms <= 0
        or args.peer_ready_timeout_ms <= 0
        or args.runtime_settle_ms < 0
        or args.decoded_fixture_bytes <= 0
        or args.decoded_fixture_bytes > 240 * 1024
        or args.peer_chunk_bytes <= 0
        or args.peer_chunk_delay_ms < 0
        or args.raw_payload_packet_bytes <= 0
        or args.raw_payload_interpacket_delay_ms < 0
    ):
        raise RuntimeError("invalid OTA HTTP gate argument")

    WORK_ROOT.mkdir(parents=True, exist_ok=True)
    work_dir = Path(tempfile.mkdtemp(prefix="ra4-ota-http-", dir=WORK_ROOT))
    package_path = work_dir / "ra4.ota"
    package = make_literal_ota(args.decoded_fixture_bytes)
    package_path.write_bytes(package)
    if args.source_only:
        try:
            if args.https:
                create_tls_material(work_dir, ipaddress.IPv4Address("127.0.0.1"))
            compile_proxy(args)
            protocol = "HTTPS" if args.https else "HTTP"
            print(f"RA4_OTA_{protocol}_FIXTURE_BYTES={len(package)}")
            print(f"RA4_OTA_{protocol}_SOURCE_GATE_PASS")
            return 0
        finally:
            shutil.rmtree(work_dir, ignore_errors=True)

    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    ssid, password = (None,None) if args.softap else load_private_credentials(args.credential_env)
    final_scan.require_final_carrier_bridge(args.bridge_port)
    network = None
    if args.softap:
        from ota_softap_test_network import OtaSoftApNetwork
        network=OtaSoftApNetwork(args,work_dir)
    host = network.host if network else windows_wlan_address()
    response_timeout = args.response_timeout_ms / 1000.0
    protocol = "HTTPS" if args.https else "HTTP"
    peer_marker = f"{protocol}_OTA_PEER"

    print(f"BL616CL final carrier: {args.bridge_port} (2341:1002 interface 00)")
    print(f"Target: BL616CL {protocol}/LittleFS OTA cache only; RA4M1 application Flash is not written.")
    print("Privacy: STA credentials, temporary CA/private key, and raw AT payloads stay out of repository logs.")

    bridge: serial.Serial | None = None
    peer: subprocess.Popen[str] | None = None
    peer_output = ""
    proxy_uploaded = False
    sta_requested = False
    primary_error: Exception | None = None
    statuses: list[int] = []
    progress_samples: list[int] = []
    try:
        image = compile_proxy(args)
        if network:
            upload_proxy(args,image);proxy_uploaded=True
            bridge=open_bridge(args.bridge_port)
            network.start(bridge,response_timeout)
            sta_requested=True
        certificate: Path | None = None
        key: Path | None = None
        ca_certificate = b""
        if args.https:
            certificate, key = create_tls_material(work_dir, host)
            ca_certificate = certificate.read_bytes()
            if not ca_certificate:
                raise RuntimeError("temporary TLS CA is empty")
        peer = launch_windows_peer(
            work_dir,
            host,
            package_path,
            args.peer_chunk_bytes,
            args.peer_chunk_delay_ms,
            args.https,
            certificate,
            key,
            tcp_diagnostics=args.tcp_diagnostics,
            tcp_nodelay=args.peer_tcp_nodelay,
        )
        peer_output = wait_for_peer_ready(
            peer,
            peer_marker,
            args.peer_ready_timeout_ms / 1000.0,
        )

        if not proxy_uploaded:
            upload_proxy(args, image)
            proxy_uploaded = True
            bridge = open_bridge(args.bridge_port)
        bridge.reset_input_buffer()

        if args.https:
            print("RA4_OTA_HTTPS_CA_UPLOAD_STARTED")
            require_exact(
                transact_raw_payload(
                    bridge,
                    (
                        f"AT+OTASETCAROOT={len(ca_certificate)}\r\n"
                    ).encode("ascii"),
                    ca_certificate,
                    args.raw_payload_packet_bytes,
                    args.raw_payload_interpacket_delay_ms / 1000.0,
                    response_timeout,
                ),
                b"OK\r\n",
                "OTASETCAROOT",
            )
            print("RA4_OTA_HTTPS_CA_UPLOAD_PASS")

        if not network:
            print(f"RA4_OTA_{protocol}_STA_CONNECT_STARTED")
            require_exact(
                transact(bridge, b"AT+WIFIMODE=1\r\n", response_timeout),
                b"+WIFIMODE: \r\nOK\r\n",
                "WIFIMODE=1",
            )
            require_exact(
                transact(bridge, b"AT+RECONNECT=0\r\n", response_timeout),
                b"OK\r\n",
                "RECONNECT=0",
            )
            begin_sta = (
                "AT+BEGINSTA="
                + quote_at_argument(ssid)
                + ("," + quote_at_argument(password) if password else "")
                + "\r\n"
            ).encode("ascii")
            response = transact(bridge, begin_sta, response_timeout)
            if re.fullmatch(rb"^\+BEGINSTA: [0-9]{1,3}\r\nOK\r\n$", response) is None:
                raise RuntimeError("BEGINSTA did not match the expected AT framing")
            sta_requested = True
            statuses = wait_for_sta(
                bridge,
                args.sta_timeout_ms / 1000.0,
                response_timeout,
            )
            print(f"RA4_OTA_{protocol}_STA_CONNECT_PASS")
        request = (
            f"AT+{'OTADOWNLOAD' if args.synchronous else 'OTADOWNLOADSTART'}="
            f"{'https' if args.https else 'http'}://{host}:{OTA_PORT}/ra4.ota,{OTA_PATH}\r\n"
        ).encode("ascii")
        print(f"RA4_OTA_{protocol}_DOWNLOAD_STARTED")
        require_exact(
            (
                transact_synchronous_download_with_telemetry(
                    bridge,
                    request,
                    args.download_timeout_ms / 1000.0,
                    tcp_diagnostics=args.tcp_diagnostics,
                )
                if args.synchronous
                else transact(bridge, request, response_timeout)
            ),
            (
                (
                    f"+OTADOWNLOAD: {args.decoded_fixture_bytes}\r\nOK\r\n"
                ).encode("ascii")
                if args.synchronous
                else b"+OTADOWNLOADSTART: 0\r\nOK\r\n"
            ),
            "OTADOWNLOAD" if args.synchronous else "OTADOWNLOADSTART",
        )

        if args.synchronous:
            progress_samples.append(len(package))
        else:
            deadline = time.monotonic() + args.download_timeout_ms / 1000.0
            while time.monotonic() < deadline:
                response = transact(
                    bridge,
                    b"AT+OTADOWNLOADPROGRESS\r\n",
                    response_timeout,
                )
                match = PROGRESS_PATTERN.fullmatch(response)
                if match is None:
                    raise RuntimeError("OTADOWNLOADPROGRESS did not match expected framing")
                progress = int(match.group(1))
                progress_samples.append(progress)
                if progress == len(package):
                    break
                if progress > len(package):
                    raise RuntimeError("OTADOWNLOADPROGRESS exceeded HTTP Content-Length")
                time.sleep(0.1)
            else:
                raise RuntimeError(f"OTA {protocol} download did not reach Content-Length")

        require_exact(
            transact(bridge, b"AT+OTAVERIFY\r\n", response_timeout),
            b"+OTAVERIFY: 0\r\nOK\r\n",
            "OTAVERIFY",
        )
        if peer.poll() is None:
            peer.wait(timeout=5)
        if peer.stdout is not None:
            peer_output += peer.stdout.read()
        if args.https and f"{peer_marker}:HANDSHAKE_PASS" not in peer_output:
            raise RuntimeError("HTTPS peer did not confirm the TLS handshake")
        if f"{peer_marker}:REQUEST" not in peer_output:
            raise RuntimeError(f"{protocol} peer did not observe the downloader request")
        if f"{peer_marker}:PAYLOAD_SENT" not in peer_output:
            raise RuntimeError(f"{protocol} peer did not complete the fixture stream")

        print(f"RA4_OTA_{protocol}_PACKAGE_BYTES={len(package)}")
        print(f"RA4_OTA_{protocol}_STA_STATUSES={','.join(map(str, statuses))}")
        print(
            f"RA4_OTA_{protocol}_PROGRESS_SAMPLES="
            + ",".join(map(str, progress_samples))
        )
        print(f"RA4_OTA_{protocol}_DOWNLOAD_VERIFY_PASS")
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
        if args.tcp_diagnostics and proxy_uploaded:
            try:
                print(capture_ota_download_telemetry(tcp_diagnostics=True), flush=True)
            except (OSError, RuntimeError, subprocess.SubprocessError):
                print("RA4_OTA_FAILURE_TELEMETRY_UNAVAILABLE", flush=True)
    finally:
        if bridge is not None and bridge.is_open:
            if sta_requested:
                try:
                    require_exact(
                        transact(bridge, b"AT+DISCONNECT\r\n", response_timeout),
                        b"+DISCONNECT: \r\nOK\r\n",
                        "cleanup DISCONNECT",
                    )
                    print(f"RA4_OTA_{protocol}_CLEANUP_STA_DISCONNECT_SENT")
                except (OSError, RuntimeError, serial.SerialException):
                    print(
                        f"RA4_OTA_{protocol}_CLEANUP_STA_DISCONNECT_FAILED",
                        file=sys.stderr,
                    )
            bridge.close()
        if peer is not None:
            stop_windows_peer(peer, work_dir)
            if peer.stdout is not None:
                peer_output += peer.stdout.read()
        if f"{peer_marker}:HANDSHAKE_PASS" in peer_output:
            print(f"{peer_marker}_HANDSHAKE_OBSERVED")
        if f"{peer_marker}:REQUEST" in peer_output:
            print(f"{peer_marker}_REQUEST_OBSERVED")
        if f"{peer_marker}:PAYLOAD_SENT" in peer_output:
            print(f"{peer_marker}_PAYLOAD_SENT")
        for line in peer_output.splitlines():
            if line.startswith((f"{peer_marker}:SEND_PROGRESS=", f"{peer_marker}:TCP_INFO")):
                print(line)
        if network:
            try:network.close()
            except (OSError,RuntimeError,subprocess.SubprocessError) as error:
                if primary_error is None:primary_error=error
        if proxy_uploaded:
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
                    primary_error = RuntimeError("final-carrier RA4M1 scan restore failed")
        shutil.rmtree(work_dir, ignore_errors=True)

    if primary_error is not None:
        raise primary_error
    print(f"RA4_OTA_{protocol}_FINAL_CARRIER_PASS")
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
