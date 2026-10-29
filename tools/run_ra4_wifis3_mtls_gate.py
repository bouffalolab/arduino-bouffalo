#!/usr/bin/env python3
"""Validate WiFiSSLClient mTLS with a SoftSE-generated BL616CL private key.

The runner first writes a temporary RA4M1 key-generation fixture. It obtains
only the P-256 public key, signs a client certificate for it with a temporary
private CA, then writes a WiFiSSLClient fixture that calls setEccSlot(). The
Windows peer requires that client certificate. Private CA material and STA
credentials stay in ignored build paths or process memory.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import ipaddress
import json
import shutil
import subprocess
import sys
import tempfile
import textwrap
import time
from pathlib import Path

import serial
from cryptography import x509
from cryptography.hazmat.primitives import hashes, serialization
from cryptography.hazmat.primitives.asymmetric import ec
from cryptography.x509.oid import NameOID

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_bl616cl_apsta_gate import DEFAULT_CREDENTIAL_ENV, load_private_credentials
from run_ra4_bossac_final_carrier import require_final_carrier_bridge
from run_ra4_wifis3_scan_smoke import capture_runtime
import run_ra4_wifis3_scan_final_carrier as final_scan
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT_DIR = Path(__file__).resolve().parents[1]
FQBN = "arduino:renesas_uno:unor4wifi"
SAMBA_RUNNER = ROOT_DIR / "tools" / "run_ra4_bossac_final_carrier.py"
KEYGEN_SKETCH = ROOT_DIR / "examples" / "ra4-wifis3-mtls-keygen"
CLIENT_TEMPLATE = (
    ROOT_DIR
    / "examples"
    / "ra4-wifis3-mtls-client-smoke"
    / "ra4-wifis3-mtls-client-smoke.ino.in"
)
WORK_ROOT = ROOT_DIR / "build" / "ra4-wifis3-mtls-client-smoke"
TLS_HOSTNAME = "tls.bl616cl.test"
TLS_PORT = 18443
EXPECTED_REQUEST = b"bl616cl-mtls-ping"
EXPECTED_REPLY = b"bl616cl-mtls-echo"
CERTIFICATE_NOT_BEFORE = datetime(1970, 1, 1, tzinfo=timezone.utc)
CERTIFICATE_NOT_AFTER = datetime(2040, 1, 1, tzinfo=timezone.utc)

WINDOWS_SERVER = r"""
import argparse
import socket
import ssl
import struct
import threading

EXPECTED_HOSTNAME = "tls.bl616cl.test"
EXPECTED_REQUEST = b"bl616cl-mtls-ping"
EXPECTED_REPLY = b"bl616cl-mtls-echo"
EXPECTED_SNI = EXPECTED_HOSTNAME
EVENT_LOG = None


def emit(message):
    print(message, flush=True)
    if EVENT_LOG:
        with open(EVENT_LOG, "a", encoding="ascii") as event_log:
            event_log.write(message + "\n")


def decode_dns_name(packet, offset):
    labels = []
    while True:
        if offset >= len(packet):
            raise ValueError("DNS name exceeds packet")
        length = packet[offset]
        offset += 1
        if length == 0:
            return ".".join(labels), offset
        if length & 0xC0:
            raise ValueError("compressed DNS query is unsupported")
        if offset + length > len(packet):
            raise ValueError("DNS label exceeds packet")
        labels.append(packet[offset:offset + length].decode("ascii"))
        offset += length


def dns_server(bind_ip, answer_ip, ready, stop):
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
                if len(packet) < 12:
                    emit("MTLS_DNS_PEER:QUERY_MALFORMED")
                    continue
                question, offset = decode_dns_name(packet, 12)
                if offset + 4 > len(packet):
                    emit("MTLS_DNS_PEER:QUERY_MALFORMED")
                    continue
                qtype, qclass = struct.unpack("!HH", packet[offset:offset + 4])
                question_end = offset + 4
                emit(
                    "MTLS_DNS_PEER:QUERY_SEEN="
                    + question
                    + ","
                    + str(qtype)
                    + ","
                    + str(qclass)
                )
                flags = 0x8180 if question == EXPECTED_HOSTNAME and qtype == 1 and qclass == 1 else 0x8183
                answers = 1 if flags == 0x8180 else 0
                if answers:
                    emit("MTLS_DNS_PEER:QUERY_RECEIVED")
                response = bytearray(packet[:2])
                response += struct.pack("!HHHHH", flags, 1, answers, 0, 0)
                response += packet[12:question_end]
                if answers:
                    response += b"\xc0\x0c"
                    response += struct.pack("!HHIH", 1, 1, 30, 4)
                    response += socket.inet_aton(answer_ip)
                link.sendto(response, client)
            except (UnicodeError, ValueError, OSError):
                emit("MTLS_DNS_PEER:QUERY_PARSE_ERROR")
                continue
    finally:
        link.close()


def tls_server(bind_ip, port, ca, cert, key, optional_client_certificate, ready, stop):
    sni = {"name": "", "ok": False}
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile=cert, keyfile=key)
    context.load_verify_locations(cafile=ca)
    context.verify_mode = (
        ssl.CERT_OPTIONAL if optional_client_certificate else ssl.CERT_REQUIRED
    )
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.set_ciphers("ECDHE-ECDSA-AES128-GCM-SHA256")

    def on_sni(_socket, server_name, _context):
        sni["name"] = server_name or ""
        sni["ok"] = server_name == EXPECTED_SNI

    context.set_servername_callback(on_sni)
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((bind_ip, port))
    listener.listen(1)
    listener.settimeout(0.2)
    ready.set()
    try:
        while not stop.is_set():
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                continue
            emit("MTLS_DNS_PEER:TCP_ACCEPTED")
            with connection:
                try:
                    connection.settimeout(3.0)
                    emit("MTLS_DNS_PEER:HANDSHAKE_STARTED")
                    with context.wrap_socket(connection, server_side=True) as secure:
                        cipher = secure.cipher()
                        emit(
                            "MTLS_DNS_PEER:NEGOTIATED="
                            + str(secure.version())
                            + ","
                            + str(cipher[0] if cipher else "none")
                        )
                        emit(
                            "MTLS_DNS_PEER:SNI="
                            + (sni["name"] or "<missing>")
                        )
                        emit(
                            "MTLS_DNS_PEER:SNI_PASS="
                            + ("1" if sni["ok"] else "0")
                        )
                        peer = secure.getpeercert()
                        if not peer:
                            emit("MTLS_DNS_PEER:CLIENT_CERT_MISSING")
                        else:
                            emit("MTLS_DNS_PEER:CLIENT_CERT_PASS")
                        request = secure.recv(128)
                        if sni["ok"] and request == EXPECTED_REQUEST:
                            secure.sendall(EXPECTED_REPLY)
                            emit("MTLS_DNS_PEER:EXCHANGE_PASS")
                            return
                        emit("MTLS_DNS_PEER:EXCHANGE_REJECTED")
                        return
                except ssl.SSLError as error:
                    reason = getattr(error, "reason", None) or "unknown"
                    emit(f"MTLS_DNS_PEER:HANDSHAKE_ERROR={reason}")
                    emit("MTLS_DNS_PEER:HANDSHAKE_REJECTED")
                    continue
                except OSError:
                    emit("MTLS_DNS_PEER:TCP_ERROR")
                    continue
    finally:
        listener.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind-ip", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--ca", required=True)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--event-log")
    parser.add_argument("--expected-sni", required=True)
    parser.add_argument("--optional-client-certificate", action="store_true")
    args = parser.parse_args()
    global EVENT_LOG
    EVENT_LOG = args.event_log
    global EXPECTED_SNI
    EXPECTED_SNI = args.expected_sni
    stop = threading.Event()
    dns_ready = threading.Event()
    tls_ready = threading.Event()
    dns = threading.Thread(
        target=dns_server,
        args=(args.bind_ip, args.bind_ip, dns_ready, stop),
        daemon=True,
    )
    tls = threading.Thread(
        target=tls_server,
        args=(
            args.bind_ip,
            args.port,
            args.ca,
            args.cert,
            args.key,
            args.optional_client_certificate,
            tls_ready,
            stop,
        ),
        daemon=True,
    )
    dns.start()
    tls.start()
    if not dns_ready.wait(5) or not tls_ready.wait(5):
        raise RuntimeError("peer startup timed out")
    emit("MTLS_DNS_PEER:READY")
    tls.join()
    stop.set()
    dns.join(timeout=2)


if __name__ == "__main__":
    main()
"""


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def wsl_unc_path(path: Path) -> str:
    return "\\\\wsl.localhost\\Ubuntu-22.04" + str(path.resolve())


def pem_c_literal(pem: str) -> str:
    return "\n".join(
        '"' + line.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'
        for line in pem.splitlines(keepends=True)
    )


def der_c_literal(der: bytes) -> str:
    lines = []
    for offset in range(0, len(der), 12):
        lines.append(
            "  " + ", ".join(f"0x{value:02X}" for value in der[offset : offset + 12]) + ","
        )
    return "\n".join(lines)


def encode_start_value(value: str) -> str:
    return value.encode("ascii", "strict").hex()


def capture_until(
    link: serial.Serial,
    required: tuple[str, ...],
    failure_prefix: str,
    timeout_seconds: float,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        received.extend(link.read(512))
        if len(received) > 8192:
            raise RuntimeError("serial marker capture exceeded its byte limit")
        text = received.decode("ascii", errors="replace").replace("\x00", "")
        if failure_prefix in text:
            raise MarkerCaptureError(text)
        if all(marker in text for marker in required):
            return text
    raise RuntimeError("required RA4M1 mTLS markers were not observed")


class MarkerCaptureError(RuntimeError):
    def __init__(self, captured: str) -> None:
        super().__init__("RA4M1 mTLS fixture reported failure")
        self.captured = captured


def print_markers(label: str, text: str) -> None:
    markers = [
        line.strip()
        for line in text.replace("\x00", "").splitlines()
        if line.strip().startswith("RA4_MTLS_")
    ]
    if markers:
        print(f"{label}_BEGIN")
        for marker in markers:
            print(marker)
        print(f"{label}_END")


def print_peer_markers(text: str) -> None:
    markers = [
        line.strip()
        for line in text.replace("\x00", "").splitlines()
        if line.strip().startswith("MTLS_DNS_PEER:")
    ]
    if markers:
        print("MTLS_DNS_PEER_MARKERS_BEGIN")
        for marker in markers:
            print(marker)
        print("MTLS_DNS_PEER_MARKERS_END")


def windows_network() -> tuple[ipaddress.IPv4Address, ipaddress.IPv4Address]:
    command = (
        '$ErrorActionPreference = "Stop"; '
        'Get-NetIPConfiguration | '
        'Where-Object { $_.InterfaceAlias -eq "WLAN" -and $_.IPv4Address -and $_.IPv4DefaultGateway } | '
        'ForEach-Object { [PSCustomObject]@{ '
        'Address=$_.IPv4Address.IPAddress; Gateway=$_.IPv4DefaultGateway.NextHop; '
        'PrefixLength=$_.IPv4Address.PrefixLength } } | ConvertTo-Json -Compress'
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
        gateway = ipaddress.IPv4Address(data["Gateway"])
        prefix_length = int(data["PrefixLength"])
        network = ipaddress.IPv4Network(f"{address}/{prefix_length}", strict=False)
    except (KeyError, TypeError, ValueError, json.JSONDecodeError) as error:
        raise RuntimeError("Windows WLAN does not have one usable IPv4 route") from error
    if network.prefixlen != 24:
        raise RuntimeError("mTLS gate currently requires a /24 Windows WLAN IPv4 network")
    return address, gateway


def choose_static_address(
    host: ipaddress.IPv4Address,
    gateway: ipaddress.IPv4Address,
) -> ipaddress.IPv4Address:
    for suffix in range(250, 199, -1):
        candidate = ipaddress.IPv4Address((int(host) & 0xFFFFFF00) | suffix)
        if candidate in (host, gateway):
            continue
        result = subprocess.run(
            [
                "powershell.exe",
                "-NoProfile",
                "-Command",
                (
                    "$ping = [System.Net.NetworkInformation.Ping]::new(); "
                    f"$reply = $ping.Send('{candidate}', 1000); "
                    "[Console]::WriteLine('RESULT=' + "
                    "($reply.Status -eq [System.Net.NetworkInformation.IPStatus]::Success)); "
                    "exit 0"
                ),
            ],
            check=False,
            capture_output=True,
        )
        if result.stdout.decode("ascii", errors="ignore").strip().lower() == "result=false":
            return candidate
    raise RuntimeError("no unresponsive temporary IPv4 candidate was found on the Windows WLAN")


def validate_static_address(
    candidate: ipaddress.IPv4Address,
    host: ipaddress.IPv4Address,
    gateway: ipaddress.IPv4Address,
) -> ipaddress.IPv4Address:
    network = ipaddress.IPv4Network(f"{host}/24", strict=False)
    if candidate not in network or candidate in (host, gateway):
        raise RuntimeError("requested static IPv4 address is outside the usable WLAN subnet")
    return candidate


def compile_sketch(
    arduino_cli: str,
    sketch_dir: Path,
    build_dir: Path,
) -> Path:
    run_command(
        [
            arduino_cli,
            "compile",
            "--fqbn",
            FQBN,
            "--build-path",
            str(build_dir),
            str(sketch_dir),
        ]
    )
    image = build_dir / f"{sketch_dir.name}.ino.bin"
    if not image.is_file():
        raise RuntimeError(f"compiled RA4M1 image does not exist: {image}")
    return image


def upload(bridge_port: str, image: Path, settle_ms: int) -> None:
    run_command(
        [
            sys.executable,
            str(SAMBA_RUNNER),
            "--bridge-port",
            bridge_port,
            "--image",
            str(image),
            "--runtime-settle-ms",
            str(settle_ms),
            "--attempts",
            "3",
        ]
    )


def capture_keygen(bridge_port: str, timeout_seconds: float) -> bytes:
    with serial.Serial(
        bridge_port,
        baudrate=115200,
        timeout=0.1,
        write_timeout=1.0,
    ) as bridge:
        bridge.dtr = False
        bridge.rts = False
        bridge.reset_input_buffer()
        text = capture_until(
            bridge,
            ("RA4_MTLS_KEYGEN:PUBKEY=", "RA4_MTLS_KEYGEN:RESULT=PASS"),
            "RA4_MTLS_KEYGEN:FAIL:",
            timeout_seconds,
        )
    print_markers("RA4_MTLS_KEYGEN", text)
    line = next(
        (
            item.strip()
            for item in text.splitlines()
            if item.strip().startswith("RA4_MTLS_KEYGEN:PUBKEY=")
        ),
        "",
    )
    encoded = line.partition("=")[2]
    if len(encoded) != 128:
        raise RuntimeError("RA4M1 key-generation public key has an unexpected length")
    try:
        key = bytes.fromhex(encoded)
    except ValueError as error:
        raise RuntimeError("RA4M1 key-generation public key is not hexadecimal") from error
    if len(key) != 64:
        raise RuntimeError("RA4M1 key-generation public key is not P-256 X||Y")
    return key


def create_material(
    work_dir: Path,
    public_key: bytes,
    server_ip: ipaddress.IPv4Address,
) -> tuple[Path, Path, Path]:
    ca_key = work_dir / "ca.key"
    ca_cert = work_dir / "ca.pem"
    server_key = work_dir / "server.key"
    server_cert = work_dir / "server.pem"
    client_cert = work_dir / "client.der"

    if len(public_key) != 64:
        raise RuntimeError("SoftSE public key is not P-256 X||Y")
    ca_private_key = ec.generate_private_key(ec.SECP256R1())
    ca_name = x509.Name(
        [x509.NameAttribute(NameOID.COMMON_NAME, "BL616CL mTLS Test CA")]
    )
    ca_certificate = (
        x509.CertificateBuilder()
        .subject_name(ca_name)
        .issuer_name(ca_name)
        .public_key(ca_private_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(CERTIFICATE_NOT_BEFORE)
        .not_valid_after(CERTIFICATE_NOT_AFTER)
        .add_extension(x509.BasicConstraints(ca=True, path_length=None), critical=True)
        .add_extension(
            x509.KeyUsage(
                digital_signature=True,
                content_commitment=False,
                key_encipherment=False,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=True,
                crl_sign=True,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .sign(ca_private_key, hashes.SHA256())
    )
    server_private_key = ec.generate_private_key(ec.SECP256R1())
    server_certificate = (
        x509.CertificateBuilder()
        .subject_name(
            x509.Name([x509.NameAttribute(NameOID.COMMON_NAME, TLS_HOSTNAME)])
        )
        .issuer_name(ca_name)
        .public_key(server_private_key.public_key())
        .serial_number(x509.random_serial_number())
        .not_valid_before(CERTIFICATE_NOT_BEFORE)
        .not_valid_after(CERTIFICATE_NOT_AFTER)
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(
            x509.KeyUsage(
                digital_signature=True,
                content_commitment=False,
                key_encipherment=True,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=False,
                crl_sign=False,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .add_extension(
            x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.SERVER_AUTH]),
            critical=False,
        )
        .add_extension(
            x509.SubjectAlternativeName([
                x509.DNSName(TLS_HOSTNAME),
                x509.IPAddress(server_ip),
            ]),
            critical=False,
        )
        .sign(ca_private_key, hashes.SHA256())
    )
    client_public_key = ec.EllipticCurvePublicKey.from_encoded_point(
        ec.SECP256R1(),
        b"\x04" + public_key,
    )
    client_certificate = (
        x509.CertificateBuilder()
        .subject_name(
            x509.Name(
                [x509.NameAttribute(NameOID.COMMON_NAME, "BL616CL SoftSE mTLS Client")]
            )
        )
        .issuer_name(ca_name)
        .public_key(client_public_key)
        .serial_number(x509.random_serial_number())
        .not_valid_before(CERTIFICATE_NOT_BEFORE)
        .not_valid_after(CERTIFICATE_NOT_AFTER)
        .add_extension(x509.BasicConstraints(ca=False, path_length=None), critical=True)
        .add_extension(
            x509.KeyUsage(
                digital_signature=True,
                content_commitment=False,
                key_encipherment=False,
                data_encipherment=False,
                key_agreement=False,
                key_cert_sign=False,
                crl_sign=False,
                encipher_only=False,
                decipher_only=False,
            ),
            critical=True,
        )
        .add_extension(
            x509.ExtendedKeyUsage([x509.oid.ExtendedKeyUsageOID.CLIENT_AUTH]),
            critical=False,
        )
        .sign(ca_private_key, hashes.SHA256())
    )
    ca_key.write_bytes(
        ca_private_key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption(),
        )
    )
    ca_cert.write_bytes(ca_certificate.public_bytes(serialization.Encoding.PEM))
    server_key.write_bytes(
        server_private_key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption(),
        )
    )
    server_cert.write_bytes(
        server_certificate.public_bytes(serialization.Encoding.PEM)
    )
    client_cert.write_bytes(
        client_certificate.public_bytes(serialization.Encoding.DER)
    )
    return ca_cert, server_cert, server_key


def generate_source_public_key(work_dir: Path) -> bytes:
    del work_dir
    point = ec.generate_private_key(ec.SECP256R1()).public_key().public_bytes(
        serialization.Encoding.X962,
        serialization.PublicFormat.UncompressedPoint,
    )
    if len(point) != 65 or point[0] != 0x04:
        raise RuntimeError("cryptography did not generate an uncompressed P-256 public key")
    return point[1:]


def compile_client_sketch(
    args: argparse.Namespace,
    work_dir: Path,
    ca_certificate: Path,
) -> Path:
    client_der = work_dir / "client.der"
    if not client_der.is_file():
        raise RuntimeError("temporary client certificate does not exist")
    sketch_dir = work_dir / "ra4-wifis3-mtls-client-smoke"
    sketch_dir.mkdir(parents=True, exist_ok=True)
    sketch = sketch_dir / "ra4-wifis3-mtls-client-smoke.ino"
    sketch.write_text(
        CLIENT_TEMPLATE.read_text(encoding="ascii")
        .replace(
            "@MTLS_TEST_CA_LITERAL@",
            pem_c_literal(ca_certificate.read_text(encoding="ascii")),
        )
        .replace("@MTLS_CLIENT_CERT_DER@", der_c_literal(client_der.read_bytes())),
        encoding="ascii",
    )
    return compile_sketch(args.arduino_cli, sketch_dir, work_dir / "client-build")


def launch_windows_peer(
    work_dir: Path,
    host: ipaddress.IPv4Address,
    tls_port: int,
    ca: Path,
    certificate: Path,
    key: Path,
    expected_sni: str,
    optional_client_certificate: bool,
    event_log: Path,
) -> subprocess.Popen[str]:
    script = work_dir / "windows_mtls_dns_peer.py"
    script.write_text(textwrap.dedent(WINDOWS_SERVER), encoding="ascii")
    optional_client_certificate_argument = (
        "--optional-client-certificate " if optional_client_certificate else ""
    )
    return subprocess.Popen(
        [
            "powershell.exe",
            "-NoProfile",
            "-Command",
            (
                f'& py.exe -3 "{wsl_unc_path(script)}" '
                f'--bind-ip {host} --port {tls_port} '
                f'--ca "{wsl_unc_path(ca)}" '
                f'--cert "{wsl_unc_path(certificate)}" '
                f'--key "{wsl_unc_path(key)}" '
                f'--expected-sni {expected_sni} '
                f"{optional_client_certificate_argument}"
                f'--event-log "{wsl_unc_path(event_log)}"'
            ),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def wait_for_peer_ready(peer: subprocess.Popen[str], timeout_seconds: float) -> str:
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
            if "MTLS_DNS_PEER:READY" in captured:
                return captured
    raise RuntimeError("temporary Windows mTLS/DNS peer did not become ready")


def stop_windows_peer(peer: subprocess.Popen[str], work_dir: Path) -> None:
    # WSL's process PID is not the Windows py.exe PID. Kill only descendants
    # whose command line carries this run's unique temporary work directory.
    work_dir_match = wsl_unc_path(work_dir).replace("'", "''")
    subprocess.run(
        [
            "powershell.exe",
            "-NoProfile",
            "-Command",
            (
                "$needle = '"
                + work_dir_match
                + "'; Get-CimInstance Win32_Process | "
                "Where-Object { $_.CommandLine -and "
                "$_.CommandLine.Contains($needle) } | "
                "ForEach-Object { taskkill.exe /PID $_.ProcessId /T /F "
                "| Out-Null }"
            ),
        ],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    if peer.poll() is None:
        try:
            peer.wait(timeout=5)
        except subprocess.TimeoutExpired:
            peer.kill()
            peer.wait(timeout=5)
    try:
        peer.wait(timeout=5)
    except subprocess.TimeoutExpired:
        peer.kill()
        peer.wait(timeout=5)


def read_peer_event_log(path: Path) -> str:
    if not path.is_file():
        return ""
    return path.read_text(encoding="ascii", errors="replace")


def restore_final_carrier_scan(args: argparse.Namespace) -> None:
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
    parser.add_argument("--mtls-timeout-ms", type=int, default=60000)
    parser.add_argument("--peer-ready-timeout-ms", type=int, default=10000)
    parser.add_argument("--paired-runtime-settle-ms", type=int, default=2000)
    parser.add_argument("--ra4-ready-timeout-ms", type=int, default=15000)
    parser.add_argument("--tls-port", type=int, default=TLS_PORT)
    parser.add_argument("--static-address", type=ipaddress.IPv4Address)
    parser.add_argument(
        "--direct-ip",
        action="store_true",
        help="skip DNS and validate mTLS by connecting directly to the peer IP",
    )
    parser.add_argument(
        "--without-client-credentials",
        action="store_true",
        help=(
            "diagnostic only: skip WiFiSSLClient.setEccSlot() while keeping "
            "the same temporary CA, server certificate, and network path"
        ),
    )
    parser.add_argument("--source-only", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.mtls_timeout_ms <= 0
        or args.peer_ready_timeout_ms <= 0
        or args.paired_runtime_settle_ms < 0
        or args.ra4_ready_timeout_ms <= 0
        or args.tls_port <= 0
        or args.tls_port > 65535
    ):
        raise RuntimeError("invalid timeout")

    WORK_ROOT.mkdir(parents=True, exist_ok=True)
    work_dir = Path(tempfile.mkdtemp(prefix="unor4-mtls-gate-", dir=WORK_ROOT))
    if args.source_only:
        try:
            image = compile_sketch(
                args.arduino_cli,
                KEYGEN_SKETCH,
                work_dir / "keygen-build",
            )
            if not image.is_file():
                raise RuntimeError("key-generation source fixture did not compile")
            public_key = generate_source_public_key(work_dir)
            ca, _, _ = create_material(
                work_dir,
                public_key,
                ipaddress.IPv4Address("192.0.2.1"),
            )
            compile_client_sketch(args, work_dir, ca)
            print("RA4_WIFIS3_MTLS_SOURCE_GATE_PASS")
            return 0
        finally:
            shutil.rmtree(work_dir, ignore_errors=True)

    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(args.bridge_port)
    ssid, password = load_private_credentials(args.credential_env)
    if password is None:
        raise RuntimeError("mTLS gate requires a private WPA password")
    host, gateway = windows_network()
    static_address = (
        validate_static_address(args.static_address, host, gateway)
        if args.static_address is not None
        else choose_static_address(host, gateway)
    )

    print(f"BL616CL final carrier: {args.bridge_port} (2341:1002 interface 00)")
    print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Privacy: STA credentials and temporary CA/key/certificate material stay out of repository logs.")

    peer = None
    peer_event_log = work_dir / "windows_mtls_dns_peer.events"
    peer_output = ""
    ra4_modified = False
    primary_error: Exception | None = None
    try:
        keygen_image = compile_sketch(
            args.arduino_cli,
            KEYGEN_SKETCH,
            work_dir / "keygen-build",
        )
        upload(args.bridge_port, keygen_image, args.paired_runtime_settle_ms)
        ra4_modified = True
        public_key = capture_keygen(
            args.bridge_port,
            args.ra4_ready_timeout_ms / 1000.0,
        )

        ca, server_certificate, server_key = create_material(
            work_dir,
            public_key,
            host,
        )
        client_image = compile_client_sketch(args, work_dir, ca)
        peer = launch_windows_peer(
            work_dir,
            host,
            args.tls_port,
            ca,
            server_certificate,
            server_key,
            str(host) if args.direct_ip else TLS_HOSTNAME,
            args.without_client_credentials,
            peer_event_log,
        )
        peer_output = wait_for_peer_ready(
            peer,
            args.peer_ready_timeout_ms / 1000.0,
        )

        upload(args.bridge_port, client_image, args.paired_runtime_settle_ms)
        with serial.Serial(
            args.bridge_port,
            baudrate=115200,
            timeout=0.1,
            write_timeout=1.0,
        ) as bridge:
            bridge.dtr = False
            bridge.rts = False
            bridge.reset_input_buffer()
            ready = capture_until(
                bridge,
                ("RA4_MTLS_CLIENT:READY",),
                "RA4_MTLS_CLIENT:RESULT=FAIL:",
                args.ra4_ready_timeout_ms / 1000.0,
            )
            print_markers("RA4_MTLS_CLIENT_STARTUP", ready)
            bridge.reset_input_buffer()
            command = (
                (
                    "STARTIPNOCRED "
                    if args.direct_ip and args.without_client_credentials
                    else "STARTNOCRED "
                    if args.without_client_credentials
                    else "STARTIP "
                    if args.direct_ip
                    else "START "
                )
                + encode_start_value(ssid)
                + " "
                + encode_start_value(password)
                + " "
                + TLS_HOSTNAME
                + " "
                + str(static_address)
                + " "
                + str(host)
                + " "
                + str(gateway)
                + " "
                + str(args.tls_port)
                + "\n"
            )
            bridge.write(command.encode("ascii"))
            bridge.flush()
            required_markers = (
                (
                    "RA4_MTLS_CLIENT:START_RECEIVED",
                    "RA4_MTLS_CLIENT:SOFTSE_KEY_PASS",
                    "RA4_MTLS_CLIENT:WIFI_CONNECTED",
                    "RA4_MTLS_CLIENT:TCP_IP_PROBE=PASS",
                    "RA4_MTLS_CLIENT:DNS_BYPASS",
                    "RA4_MTLS_CLIENT:TLS_CONNECT_PASS",
                    "RA4_MTLS_CLIENT:TLS_WRITE_PASS",
                    "RA4_MTLS_CLIENT:TLS_READ_PASS",
                    "RA4_MTLS_CLIENT:TLS_CLOSE_PASS",
                    "RA4_MTLS_CLIENT:RESULT=PASS",
                )
                if args.direct_ip
                else (
                    "RA4_MTLS_CLIENT:START_RECEIVED",
                    "RA4_MTLS_CLIENT:SOFTSE_KEY_PASS",
                    "RA4_MTLS_CLIENT:WIFI_CONNECTED",
                    "RA4_MTLS_CLIENT:TCP_IP_PROBE=PASS",
                    "RA4_MTLS_CLIENT:DNS_PASS",
                    "RA4_MTLS_CLIENT:DNS_PEER_PASS",
                    "RA4_MTLS_CLIENT:TLS_CONNECT_PASS",
                    "RA4_MTLS_CLIENT:TLS_WRITE_PASS",
                    "RA4_MTLS_CLIENT:TLS_READ_PASS",
                    "RA4_MTLS_CLIENT:TLS_CLOSE_PASS",
                    "RA4_MTLS_CLIENT:RESULT=PASS",
                )
            )
            capture = capture_until(
                bridge,
                required_markers,
                "RA4_MTLS_CLIENT:RESULT=FAIL:",
                args.mtls_timeout_ms / 1000.0,
            )
        print_markers("RA4_MTLS_CLIENT", capture)
        if peer.poll() is None:
            peer.wait(timeout=5)
        peer_output += peer.stdout.read() if peer.stdout is not None else ""
        peer_output += read_peer_event_log(peer_event_log)
        if "MTLS_DNS_PEER:EXCHANGE_PASS" not in peer_output:
            raise RuntimeError("temporary mTLS peer did not complete the application exchange")
        if args.without_client_credentials:
            if "MTLS_DNS_PEER:CLIENT_CERT_MISSING" not in peer_output:
                raise RuntimeError("mTLS control peer unexpectedly received a client certificate")
        elif "MTLS_DNS_PEER:CLIENT_CERT_PASS" not in peer_output:
            raise RuntimeError("temporary mTLS peer did not verify the SoftSE client certificate")
        print("RA4_WIFIS3_MTLS_GATE_PASS")
    except MarkerCaptureError as error:
        print_markers("RA4_MTLS_CLIENT_FAILURE", error.captured)
        primary_error = error
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
    finally:
        if peer is not None:
            # Give the peer a short window to flush TCP/SNI/alert markers
            # before the cleanup task terminates the Windows process.
            if peer.poll() is None:
                try:
                    peer.wait(timeout=3.5)
                except subprocess.TimeoutExpired:
                    pass
            stop_windows_peer(peer, work_dir)
        if peer is not None and peer.stdout is not None:
            peer_output += peer.stdout.read()
        peer_output += read_peer_event_log(peer_event_log)
        print_peer_markers(peer_output)
        if ra4_modified:
            try:
                restore_final_carrier_scan(args)
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
