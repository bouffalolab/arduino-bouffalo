#!/usr/bin/env python3
"""Run the BL616CL WiFiSSLClient gate against a temporary Windows TLS/DNS peer.

The TLS CA, private key, generated RA4M1 Sketch, STA credentials, local
address and application payload exist only in ignored build paths or process
memory. The Windows peer replies only after receiving the expected SNI.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import ipaddress
import json
import os
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
from cryptography.hazmat.primitives.asymmetric import ec, rsa
from cryptography.x509.oid import ExtendedKeyUsageOID, NameOID

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_bl616cl_apsta_gate import DEFAULT_CREDENTIAL_ENV, load_private_credentials
import run_ra4_wifis3_scan_final_carrier as final_scan
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT_DIR = Path(__file__).resolve().parents[1]
FQBN = "arduino:renesas_uno:unor4wifi"
SAMBA_RUNNER = ROOT_DIR / "tools" / "run_ra4_bossac_final_carrier.py"
WIFIS3_OVERLAY_PREPARER = (
    ROOT_DIR / "tools" / "prepare_wifis3_result_error_overlay.py"
)
TEMPLATE = (
    ROOT_DIR
    / "examples"
    / "ra4-wifis3-tls-client-smoke"
    / "ra4-wifis3-tls-client-smoke.ino.in"
)
WORK_ROOT = ROOT_DIR / "build" / "ra4-wifis3-tls-client-smoke"
TLS_HOSTNAME = "tls.bl616cl.test"
# Keep the DNS listener on 53, but use a separate high TCP port for TLS so the
# gate can prove TCP reachability without depending on the Windows DNS rule.
TLS_PORT = 18443
EXPECTED_REQUEST = b"bl616cl-tls-ping"
EXPECTED_REPLY = b"bl616cl-tls-echo"
CERTIFICATE_NOT_BEFORE = datetime(1970, 1, 1, tzinfo=timezone.utc)
CERTIFICATE_NOT_AFTER = datetime(2040, 1, 1, tzinfo=timezone.utc)


WINDOWS_SERVER = r"""
import argparse
import socket
import ssl
import struct
import threading

EXPECTED_HOSTNAME = "tls.bl616cl.test"
EXPECTED_REQUEST = b"bl616cl-tls-ping"
EXPECTED_REPLY = b"bl616cl-tls-echo"
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
                    emit("TLS_DNS_PEER:QUERY_MALFORMED")
                    continue
                question, offset = decode_dns_name(packet, 12)
                if offset + 4 > len(packet):
                    emit("TLS_DNS_PEER:QUERY_MALFORMED")
                    continue
                qtype, qclass = struct.unpack("!HH", packet[offset:offset + 4])
                question_end = offset + 4
                emit(
                    "TLS_DNS_PEER:QUERY_SEEN="
                    + question
                    + ","
                    + str(qtype)
                    + ","
                    + str(qclass)
                )
                flags = 0x8180 if question == EXPECTED_HOSTNAME and qtype == 1 and qclass == 1 else 0x8183
                answers = 1 if flags == 0x8180 else 0
                if answers:
                    emit("TLS_DNS_PEER:QUERY_RECEIVED")
                response = bytearray(packet[:2])
                response += struct.pack("!HHHHH", flags, 1, answers, 0, 0)
                response += packet[12:question_end]
                if answers:
                    response += b"\xc0\x0c"
                    response += struct.pack("!HHIH", 1, 1, 30, 4)
                    response += socket.inet_aton(answer_ip)
                link.sendto(response, client)
            except (UnicodeError, ValueError, OSError):
                emit("TLS_DNS_PEER:QUERY_PARSE_ERROR")
                continue
    finally:
        link.close()


def tls_server(bind_ip, port, cert, key, certificate_key_type, peer_mode, ready, stop):
    sni = {"name": "", "ok": False}
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile=cert, keyfile=key)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.set_ciphers(
        "ECDHE-RSA-AES128-GCM-SHA256"
        if certificate_key_type == "rsa"
        else "ECDHE-ECDSA-AES128-GCM-SHA256"
    )

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
            emit("TLS_DNS_PEER:TCP_ACCEPTED")
            with connection:
                try:
                    connection.settimeout(3.0)
                    if peer_mode == "stall":
                        saw_tls_bytes = False
                        while not stop.is_set():
                            try:
                                data = connection.recv(4096)
                            except socket.timeout:
                                continue
                            if not data:
                                emit(
                                    "TLS_DNS_PEER:"
                                    + (
                                        "HANDSHAKE_STALL_CLIENT_CLOSED"
                                        if saw_tls_bytes
                                        else "TCP_PROBE_CLOSED"
                                    )
                                )
                                break
                            if not saw_tls_bytes:
                                saw_tls_bytes = True
                                emit("TLS_DNS_PEER:HANDSHAKE_STALL_STARTED")
                        if saw_tls_bytes:
                            return
                        continue
                    emit("TLS_DNS_PEER:HANDSHAKE_STARTED")
                    with context.wrap_socket(connection, server_side=True) as secure:
                        cipher = secure.cipher()
                        emit(
                            "TLS_DNS_PEER:NEGOTIATED="
                            + str(secure.version())
                            + ","
                            + str(cipher[0] if cipher else "none")
                        )
                        emit(
                            "TLS_DNS_PEER:SNI="
                            + (sni["name"] or "<missing>")
                        )
                        emit(
                            "TLS_DNS_PEER:SNI_PASS="
                            + ("1" if sni["ok"] else "0")
                        )
                        request = secure.recv(128)
                        if sni["ok"] and request == EXPECTED_REQUEST:
                            secure.sendall(EXPECTED_REPLY)
                            emit("TLS_DNS_PEER:EXCHANGE_PASS")
                            return
                        emit("TLS_DNS_PEER:EXCHANGE_REJECTED")
                        return
                except ssl.SSLError as error:
                    reason = getattr(error, "reason", None) or "unknown"
                    emit(f"TLS_DNS_PEER:HANDSHAKE_ERROR={reason}")
                    emit("TLS_DNS_PEER:HANDSHAKE_REJECTED")
                    continue
                except OSError:
                    emit("TLS_DNS_PEER:TCP_ERROR")
                    continue
    finally:
        listener.close()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind-ip", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument(
        "--certificate-key-type",
        choices=("ec", "rsa"),
        required=True,
    )
    parser.add_argument("--event-log")
    parser.add_argument("--expected-sni", required=True)
    parser.add_argument("--peer-mode", choices=("tls", "stall"), default="tls")
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
            args.cert,
            args.key,
            args.certificate_key_type,
            args.peer_mode,
            tls_ready,
            stop,
        ),
        daemon=True,
    )
    dns.start()
    tls.start()
    if not dns_ready.wait(5) or not tls_ready.wait(5):
        raise RuntimeError("peer startup timed out")
    emit("TLS_DNS_PEER:READY")
    tls.join()
    stop.set()
    dns.join(timeout=2)


if __name__ == "__main__":
    main()
"""


class MarkerCaptureError(RuntimeError):
    def __init__(self, marker: str, captured: str) -> None:
        super().__init__(f"RA4M1 TLS smoke reported failure: {marker}")
        self.captured = captured


class MarkerCaptureTimeoutError(RuntimeError):
    def __init__(self, message: str, captured: str) -> None:
        super().__init__(message)
        self.captured = captured


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def wsl_unc_path(path: Path) -> str:
    return "\\\\wsl.localhost\\Ubuntu-22.04" + str(path.resolve())


def encode_start_value(value: str) -> str:
    return value.encode("ascii", "strict").hex()


def pem_c_literal(pem: str) -> str:
    lines = pem.splitlines(keepends=True)
    return "\n".join(
        '"' + line.replace("\\", "\\\\").replace('"', '\\"').replace("\n", "\\n") + '"'
        for line in lines
    )


def capture_until(
    link: serial.Serial,
    required: tuple[str, ...],
    failure_prefix: str,
    timeout_seconds: float,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        chunk = link.read(512)
        if not chunk:
            continue
        received.extend(chunk)
        if len(received) > 8192:
            raise RuntimeError("serial marker capture exceeded its byte limit")
        text = received.decode("ascii", errors="replace").replace("\x00", "")
        if failure_prefix in text:
            start = text.find(failure_prefix)
            end = text.find("\n", start)
            raise MarkerCaptureError(
                text[start:end if end >= 0 else len(text)].strip(),
                text,
            )
        if all(marker in text for marker in required):
            return text
    raise RuntimeError("required RA4M1 TLS markers were not observed")


def capture_failure(
    link: serial.Serial,
    required: tuple[str, ...],
    expected_failure: str,
    timeout_seconds: float,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    text = ""
    while time.monotonic() < deadline:
        chunk = link.read(512)
        if not chunk:
            continue
        received.extend(chunk)
        if len(received) > 8192:
            raise RuntimeError("serial marker capture exceeded its byte limit")
        text = received.decode("ascii", errors="replace").replace("\x00", "")
        if expected_failure in text:
            if all(marker in text for marker in required):
                return text
            raise RuntimeError("TLS failure arrived before its required diagnostics")
    raise MarkerCaptureTimeoutError(
        "expected RA4M1 TLS failure was not observed",
        text,
    )


def wait_for_ra4_ready(link: serial.Serial, timeout_seconds: float) -> None:
    startup = capture_until(
        link,
        ("RA4_TLS_CLIENT:READY",),
        "RA4_TLS_CLIENT:RESULT=FAIL:",
        timeout_seconds,
    )
    print_markers("PAIRED_TLS_CLIENT_STARTUP", startup)


def write_paced(
    link: serial.Serial,
    payload: bytes,
    packet_bytes: int,
    interpacket_delay_seconds: float,
) -> None:
    for offset in range(0, len(payload), packet_bytes):
        packet = payload[offset : offset + packet_bytes]
        written = link.write(packet)
        if written != len(packet):
            raise RuntimeError(
                f"TLS START write accepted {written}/{len(packet)} byte(s)"
            )
        link.flush()
        if offset + packet_bytes < len(payload):
            time.sleep(interpacket_delay_seconds)


def print_markers(label: str, text: str) -> None:
    markers = [
        line.strip()
        for line in text.replace("\x00", "").splitlines()
        if line.strip().startswith("RA4_TLS_CLIENT:")
    ]
    if markers:
        print(f"{label}_BEGIN")
        for marker in markers:
            print(marker)
        print(f"{label}_END")
    trace_lines = [
        line.strip()
        for line in text.replace("\x00", "").splitlines()
        if line.strip().startswith(
            ("REQUEST:", "RAW:", "<-RAW END", "ANSWER:", "Result:")
        )
    ]
    if trace_lines:
        print("PAIRED_TLS_MODEM_TRACE_BEGIN")
        for trace_line in trace_lines:
            print(trace_line)
        print("PAIRED_TLS_MODEM_TRACE_END")


def print_peer_markers(text: str) -> None:
    markers = [
        line.strip()
        for line in text.replace("\x00", "").splitlines()
        if line.strip().startswith("TLS_DNS_PEER:")
    ]
    if markers:
        print("TLS_DNS_PEER_MARKERS_BEGIN")
        for marker in markers:
            print(marker)
        print("TLS_DNS_PEER_MARKERS_END")


def windows_wlan_address() -> str:
    command = (
        '$ErrorActionPreference = "Stop"; '
        'Get-NetIPConfiguration | '
        'Where-Object { $_.InterfaceAlias -eq "WLAN" -and $_.IPv4Address -and $_.IPv4DefaultGateway } | '
        'ForEach-Object { [PSCustomObject]@{ '
        'Address=$_.IPv4Address.IPAddress } } | ConvertTo-Json -Compress'
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", command],
        check=False,
        capture_output=True,
        text=True,
        errors="replace",
    )
    if result.returncode != 0:
        raise RuntimeError("cannot query the Windows WLAN IPv4 configuration")
    try:
        data = json.loads(result.stdout)
        if isinstance(data, list):
            if len(data) != 1:
                raise ValueError()
            data = data[0]
        return str(data["Address"])
    except (KeyError, TypeError, json.JSONDecodeError) as error:
        raise RuntimeError("Windows WLAN does not have one usable IPv4 route") from error


def configure_peer_firewall(host: str, tls_port: int) -> str:
    rule_prefix = f"BL616CL-TLS-GATE-{os.getpid()}"
    udp_rule = rule_prefix + "-DNS"
    tcp_rule = rule_prefix + "-TLS"
    command = (
        '$ErrorActionPreference = "Stop"; '
        f'Remove-NetFirewallRule -DisplayName "{udp_rule}" -ErrorAction SilentlyContinue; '
        f'Remove-NetFirewallRule -DisplayName "{tcp_rule}" -ErrorAction SilentlyContinue; '
        f'New-NetFirewallRule -DisplayName "{udp_rule}" -Direction Inbound -Action Allow '
        f'-Profile Any -Protocol UDP -LocalAddress {host} -LocalPort 53 | Out-Null; '
        f'New-NetFirewallRule -DisplayName "{tcp_rule}" -Direction Inbound -Action Allow '
        f'-Profile Any -Protocol TCP -LocalAddress {host} -LocalPort {tls_port} | Out-Null; '
        'Write-Output "FIREWALL_READY"'
    )
    result = subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", command],
        check=False,
        capture_output=True,
        text=True,
        errors="replace",
    )
    if result.returncode != 0 or "FIREWALL_READY" not in result.stdout:
        detail = (result.stderr or result.stdout).strip()
        raise RuntimeError(
            "cannot create temporary Windows inbound rules; run this gate from "
            "an elevated Windows shell or allow UDP/53 and TCP/"
            f"{tls_port} to {host} ({detail})"
        )
    print("TLS_DNS_PEER:FIREWALL_READY")
    return rule_prefix


def remove_peer_firewall(rule_name: str | None) -> None:
    if not rule_name:
        return
    command = (
        '$ErrorActionPreference = "SilentlyContinue"; '
        f'Remove-NetFirewallRule -DisplayName "{rule_name}-DNS"; '
        f'Remove-NetFirewallRule -DisplayName "{rule_name}-TLS"'
    )
    subprocess.run(
        ["powershell.exe", "-NoProfile", "-Command", command],
        check=False,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
        errors="replace",
    )


def create_material(
    work_dir: Path,
    certificate_key_type: str,
    server_ip: ipaddress.IPv4Address,
) -> tuple[Path, Path, Path]:
    ca_cert = work_dir / "ca.pem"
    server_key = work_dir / "server.key"
    server_cert = work_dir / "server.pem"
    ca_private_key = ec.generate_private_key(ec.SECP256R1())
    ca_name = x509.Name(
        [x509.NameAttribute(NameOID.COMMON_NAME, "BL616CL TLS test CA")]
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
    if certificate_key_type == "rsa":
        server_private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    else:
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
            x509.ExtendedKeyUsage([ExtendedKeyUsageOID.SERVER_AUTH]),
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
    ca_cert.write_bytes(ca_certificate.public_bytes(serialization.Encoding.PEM))
    server_key.write_bytes(
        server_private_key.private_bytes(
            serialization.Encoding.PEM,
            serialization.PrivateFormat.TraditionalOpenSSL,
            serialization.NoEncryption(),
        )
    )
    server_cert.write_bytes(server_certificate.public_bytes(serialization.Encoding.PEM))
    return ca_cert, server_cert, server_key


def compile_tls_sketch(
    args: argparse.Namespace,
    work_dir: Path,
    certificate: Path,
) -> Path:
    ca_pem = certificate.read_text(encoding="ascii")
    sketch_dir = work_dir / "ra4-wifis3-tls-client-smoke"
    sketch_dir.mkdir(parents=True, exist_ok=True)
    sketch = sketch_dir / "ra4-wifis3-tls-client-smoke.ino"
    template = TEMPLATE.read_text(encoding="ascii")
    sketch.write_text(
        template
        .replace("@TLS_TEST_CA_LITERAL@", pem_c_literal(ca_pem))
        .replace("@TLS_CLIENT_TIMEOUT_MS@", str(args.tls_client_timeout_ms))
        .replace(
            "@MODEM_RESPONSE_TIMEOUT_MS@",
            str(args.modem_response_timeout_ms),
        )
        .replace(
            "@TRACE_TLS_MODEM@",
            "true" if args.trace_tls_modem else "false",
        ),
        encoding="ascii",
    )
    build_dir = work_dir / "build"
    compile_command = [
        args.arduino_cli,
        "compile",
        "--fqbn",
        FQBN,
        "--build-path",
        str(build_dir),
    ]
    if args.use_wifis3_result_error_overlay:
        overlay_library = work_dir / "libraries" / "WiFiS3"
        run_command(
            [
                sys.executable,
                str(WIFIS3_OVERLAY_PREPARER),
                "--output-library",
                str(overlay_library),
            ]
        )
        compile_command.extend(["--library", str(overlay_library)])
    compile_command.append(str(sketch_dir))
    run_command(compile_command)
    image = build_dir / "ra4-wifis3-tls-client-smoke.ino.bin"
    if not image.is_file():
        raise RuntimeError("compiled RA4M1 TLS smoke image does not exist")
    return image


def launch_windows_peer(
    work_dir: Path,
    host: str,
    tls_port: int,
    certificate: Path,
    key: Path,
    certificate_key_type: str,
    expected_sni: str,
    peer_mode: str,
    event_log: Path,
) -> subprocess.Popen[str]:
    script = work_dir / "windows_tls_dns_peer.py"
    script.write_text(textwrap.dedent(WINDOWS_SERVER), encoding="ascii")
    return subprocess.Popen(
        [
            "powershell.exe",
            "-NoProfile",
            "-Command",
            (
                f'& py.exe -3 "{wsl_unc_path(script)}" '
                f'--bind-ip {host} --port {tls_port} '
                f'--expected-sni {expected_sni} '
                f'--peer-mode {peer_mode} '
                f'--certificate-key-type {certificate_key_type} '
                f'--cert "{wsl_unc_path(certificate)}" '
                f'--key "{wsl_unc_path(key)}" '
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
            if "TLS_DNS_PEER:READY" in captured:
                return captured
    detail = "exit=" + str(peer.poll())
    for line in captured.splitlines():
        if line.startswith("TLS_DNS_PEER:STARTUP_ERROR="):
            fields = line.partition("=")[2].split(",")
            if (len(fields) == 2 and fields[0].isidentifier() and
                    fields[1].lstrip("-").isdigit()):
                detail = "startup=" + ",".join(fields)
    raise RuntimeError("temporary Windows TLS/DNS peer did not become ready (" + detail + ")")


def stop_windows_peer(peer: subprocess.Popen[str], work_dir: Path, cleanup_timeout_seconds=None) -> None:
    try:
        peer.wait(timeout=1)
        return
    except subprocess.TimeoutExpired:
        pass
    # WSL's process PID is not the Windows py.exe PID. Kill only descendants
    # whose command line carries this run's unique temporary work directory.
    work_dir_match = wsl_unc_path(work_dir).replace("'", "''")
    try:
        subprocess.run(
            [
                "powershell.exe",
                "-NoProfile",
                "-Command",
                (
                    "$needle = '"
                    + work_dir_match
                    + "'; Get-CimInstance Win32_Process | "
                    "Where-Object { $_.Name -match '^(python[0-9.]*|pythonw|py)\\.exe$' -and $_.CommandLine -and "
                    "$_.CommandLine.Contains($needle) } | "
                    "ForEach-Object { taskkill.exe /PID $_.ProcessId /T /F "
                    "| Out-Null }"
                ),
            ],
            timeout=cleanup_timeout_seconds,
            check=False,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
        )
    except subprocess.TimeoutExpired as error:
        peer.kill()
        peer.wait(timeout=5)
        raise RuntimeError("Windows TLS peer cleanup timed out") from error
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


def wait_for_peer_events(
    event_log: Path,
    required: tuple[str, ...],
    timeout_seconds: float,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    observed = ""
    while time.monotonic() < deadline:
        observed = read_peer_event_log(event_log)
        if all(marker in observed for marker in required):
            return observed
        time.sleep(0.05)
    missing = ", ".join(marker for marker in required if marker not in observed)
    raise RuntimeError(f"temporary TLS peer did not emit required events: {missing}")


def upload_tls_sketch(args: argparse.Namespace, image: Path) -> None:
    run_command(
        [
            sys.executable,
            str(SAMBA_RUNNER),
            "--bridge-port",
            args.bridge_port,
            "--image",
            str(image),
            "--runtime-settle-ms",
            str(args.paired_runtime_settle_ms),
            "--attempts",
            "3",
        ]
    )


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
    parser.add_argument("--tls-timeout-ms", type=int, default=60000)
    parser.add_argument(
        "--tls-client-timeout-ms",
        type=int,
        default=30000,
        help="WiFiSSLClient timeout embedded in the temporary RA4M1 Sketch",
    )
    parser.add_argument(
        "--modem-response-timeout-ms",
        type=int,
        default=60000,
        help=(
            "temporary RA4M1 ModemClass AT-response timeout used while "
            "WiFiSSLClient.connect() is in progress"
        ),
    )
    parser.add_argument("--peer-ready-timeout-ms", type=int, default=10000)
    parser.add_argument("--paired-runtime-settle-ms", type=int, default=2000)
    parser.add_argument("--ra4-ready-timeout-ms", type=int, default=15000)
    parser.add_argument("--start-packet-bytes", type=int, default=16)
    parser.add_argument("--start-interpacket-delay-ms", type=int, default=10)
    parser.add_argument("--tls-port", type=int, default=TLS_PORT)
    parser.add_argument(
        "--certificate-key-type",
        choices=("ec", "rsa"),
        default="ec",
        help="temporary TLS server certificate key type (default: ec)",
    )
    parser.add_argument(
        "--skip-peer-firewall",
        action="store_true",
        help=(
            "do not create temporary Windows inbound rules; use this only "
            "when the required UDP/53 and TCP gate-port rules already exist"
        ),
    )
    parser.add_argument(
        "--direct-ip",
        action="store_true",
        help="skip DNS and validate TLS by connecting directly to the peer IP",
    )
    parser.add_argument(
        "--expect-untrusted-ca",
        action="store_true",
        help=(
            "expect TLS certificate verification to fail after DNS/TCP; "
            "uses a distinct temporary CA only for the RA4M1 client"
        ),
    )
    parser.add_argument(
        "--expect-nxdomain",
        action="store_true",
        help=(
            "expect hostname resolution to fail with the test DNS peer "
            "returning NXDOMAIN; TLS must not be attempted"
        ),
    )
    parser.add_argument(
        "--expect-handshake-timeout",
        action="store_true",
        help=(
            "expect TLS handshake timeout after DNS/TCP; the peer reads the "
            "ClientHello but intentionally never replies"
        ),
    )
    parser.add_argument(
        "--use-wifis3-result-error-overlay",
        action="store_true",
        help=(
            "compile the temporary Sketch with the repository-controlled "
            "WiFiS3 RESULT_ERROR core-fix overlay; does not modify "
            "~/.arduino15"
        ),
    )
    parser.add_argument(
        "--trace-tls-modem",
        action="store_true",
        help=(
            "emit ModemClass level-1 raw AT trace during TLS configuration "
            "and connect only; STA credentials are configured before tracing"
        ),
    )
    parser.add_argument(
        "--require-ra4-deadline",
        action="store_true",
        help=(
            "in handshake-timeout mode, additionally require the RA4 "
            "WiFiSSLClient wall-clock to match the requested deadline"
        ),
    )
    parser.add_argument(
        "--source-only",
        action="store_true",
        help="Generate the temporary CA and compile the RA4M1 smoke without hardware access.",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.tls_timeout_ms <= 0
        or args.tls_client_timeout_ms < 1000
        or args.tls_client_timeout_ms > args.tls_timeout_ms
        or args.modem_response_timeout_ms < args.tls_client_timeout_ms
        or args.peer_ready_timeout_ms <= 0
        or args.paired_runtime_settle_ms < 0
        or args.ra4_ready_timeout_ms <= 0
        or args.start_packet_bytes <= 0
        or args.start_interpacket_delay_ms < 0
        or args.tls_port <= 0
        or args.tls_port > 65535
    ):
        raise RuntimeError("invalid timeout")
    if args.direct_ip and (
        args.expect_untrusted_ca
        or args.expect_nxdomain
        or args.expect_handshake_timeout
    ):
        raise RuntimeError("negative TLS/DNS gates require hostname DNS and SNI")
    if sum(
        (
            args.expect_untrusted_ca,
            args.expect_nxdomain,
            args.expect_handshake_timeout,
        )
    ) > 1:
        raise RuntimeError("select only one negative TLS/DNS gate")

    WORK_ROOT.mkdir(parents=True, exist_ok=True)
    work_dir = Path(tempfile.mkdtemp(prefix="unor4-tls-gate-", dir=WORK_ROOT))
    if args.source_only:
        try:
            ca_certificate, _, _ = create_material(
                work_dir,
                args.certificate_key_type,
                ipaddress.IPv4Address("192.0.2.1"),
            )
            compile_tls_sketch(args, work_dir, ca_certificate)
            print("RA4_WIFIS3_TLS_SOURCE_GATE_PASS")
            return 0
        finally:
            shutil.rmtree(work_dir, ignore_errors=True)

    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    ssid, password = load_private_credentials(args.credential_env)
    if password is None:
        raise RuntimeError("TLS gate requires a private WPA password")
    final_scan.require_final_carrier_bridge(args.bridge_port)
    host = windows_wlan_address()

    print(f"BL616CL final carrier: {args.bridge_port} (2341:1002 interface 00)")
    print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Privacy: STA credentials, temporary CA/private key, peer addresses, and payload stay out of repository logs.")

    bridge = None
    peer = None
    firewall_rule = None
    peer_event_log = work_dir / "windows_tls_dns_peer.events"
    peer_output = ""
    ra4_modified = False
    primary_error: Exception | None = None
    try:
        ca_certificate, server_certificate, server_key = create_material(
            work_dir,
            args.certificate_key_type,
            ipaddress.IPv4Address(host),
        )
        client_ca_certificate = ca_certificate
        if args.expect_untrusted_ca:
            untrusted_dir = work_dir / "untrusted-ca"
            untrusted_dir.mkdir()
            client_ca_certificate, _, _ = create_material(
                untrusted_dir,
                args.certificate_key_type,
                ipaddress.IPv4Address(host),
            )
        image = compile_tls_sketch(args, work_dir, client_ca_certificate)
        if args.skip_peer_firewall:
            print("TLS_DNS_PEER:FIREWALL_SKIPPED")
        else:
            firewall_rule = configure_peer_firewall(host, args.tls_port)
        requested_hostname = (
            "nxdomain.bl616cl.test"
            if args.expect_nxdomain
            else TLS_HOSTNAME
        )
        peer = launch_windows_peer(
            work_dir,
            host,
            args.tls_port,
            server_certificate,
            server_key,
            args.certificate_key_type,
            str(host) if args.direct_ip else requested_hostname,
            "stall" if args.expect_handshake_timeout else "tls",
            peer_event_log,
        )
        peer_output = wait_for_peer_ready(
            peer,
            args.peer_ready_timeout_ms / 1000.0,
        )

        upload_tls_sketch(args, image)
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
        wait_for_ra4_ready(bridge, args.ra4_ready_timeout_ms / 1000.0)
        bridge.reset_input_buffer()
        start_command = (
            ("STARTIP " if args.direct_ip else "START ")
            + encode_start_value(ssid)
            + " "
            + encode_start_value(password)
            + " "
            + requested_hostname
            + " "
            + str(host)
            + " "
            + str(args.tls_port)
            + "\n"
        )
        write_paced(
            bridge,
            start_command.encode("ascii"),
            args.start_packet_bytes,
            args.start_interpacket_delay_ms / 1000.0,
        )
        start_capture = capture_until(
            bridge,
            ("RA4_TLS_CLIENT:START_RECEIVED",),
            "RA4_TLS_CLIENT:RESULT=FAIL:",
            args.ra4_ready_timeout_ms / 1000.0,
        )
        print_markers("PAIRED_TLS_CLIENT_COMMAND", start_capture)
        required_markers = (
            (
                "RA4_TLS_CLIENT:WIFI_CONNECTED",
                "RA4_TLS_CLIENT:DNS_BYPASS",
                "RA4_TLS_CLIENT:TLS_CONNECT_PASS",
                "RA4_TLS_CLIENT:TLS_WRITE_PASS",
                "RA4_TLS_CLIENT:TLS_READ_PASS",
                "RA4_TLS_CLIENT:TLS_CLOSE_PASS",
                "RA4_TLS_CLIENT:RESULT=PASS",
            )
            if args.direct_ip
            else (
                "RA4_TLS_CLIENT:WIFI_CONNECTED",
                "RA4_TLS_CLIENT:DNS_PASS",
                "RA4_TLS_CLIENT:DNS_PEER_PASS",
                "RA4_TLS_CLIENT:TLS_CONNECT_PASS",
                "RA4_TLS_CLIENT:TLS_WRITE_PASS",
                "RA4_TLS_CLIENT:TLS_READ_PASS",
                "RA4_TLS_CLIENT:TLS_CLOSE_PASS",
                "RA4_TLS_CLIENT:RESULT=PASS",
            )
        )
        command_started_at = time.monotonic()
        if args.expect_untrusted_ca:
            capture = capture_failure(
                bridge,
                (
                    "RA4_TLS_CLIENT:WIFI_CONNECTED",
                    "RA4_TLS_CLIENT:DNS_PASS",
                    "RA4_TLS_CLIENT:DNS_PEER_PASS",
                    "RA4_TLS_CLIENT:TLS_DIAG=",
                    "RA4_TLS_CLIENT:TLS_VERIFY_FLAGS=",
                    "RA4_TLS_CLIENT:TLS_ENDPOINT=",
                ),
                "RA4_TLS_CLIENT:RESULT=FAIL:TLS_CONNECT",
                args.tls_timeout_ms / 1000.0,
            )
        elif args.expect_nxdomain:
            capture = capture_failure(
                bridge,
                (
                    "RA4_TLS_CLIENT:WIFI_CONNECTED",
                    "RA4_TLS_CLIENT:TCP_IP_PROBE=PASS",
                    "RA4_TLS_CLIENT:DNS_DIAG=",
                ),
                "RA4_TLS_CLIENT:RESULT=FAIL:DNS",
                args.tls_timeout_ms / 1000.0,
            )
        elif args.expect_handshake_timeout:
            capture = capture_failure(
                bridge,
                (
                    "RA4_TLS_CLIENT:WIFI_CONNECTED",
                    "RA4_TLS_CLIENT:DNS_PASS",
                    "RA4_TLS_CLIENT:DNS_PEER_PASS",
                    "RA4_TLS_CLIENT:TLS_CONNECT_ELAPSED_MS=",
                    "RA4_TLS_CLIENT:TLS_DIAG=",
                    "RA4_TLS_CLIENT:TLS_VERIFY_FLAGS=",
                    "RA4_TLS_CLIENT:TLS_ENDPOINT=",
                ),
                "RA4_TLS_CLIENT:RESULT=FAIL:TLS_CONNECT",
                args.tls_timeout_ms / 1000.0,
            )
        else:
            capture = capture_until(
                bridge,
                required_markers,
                "RA4_TLS_CLIENT:RESULT=FAIL:",
                args.tls_timeout_ms / 1000.0,
            )
        print_markers("PAIRED_TLS_CLIENT", capture)
        if peer.poll() is None and not (
            args.expect_untrusted_ca
            or args.expect_nxdomain
            or args.expect_handshake_timeout
        ):
            peer.wait(timeout=5)
        if args.expect_untrusted_ca:
            peer_output += wait_for_peer_events(
                peer_event_log,
                (
                    "TLS_DNS_PEER:QUERY_RECEIVED",
                    "TLS_DNS_PEER:TCP_ACCEPTED",
                    "TLS_DNS_PEER:HANDSHAKE_REJECTED",
                ),
                5.0,
            )
            print("RA4_WIFIS3_TLS_UNTRUSTED_CA_GATE_PASS")
        elif args.expect_nxdomain:
            peer_output += wait_for_peer_events(
                peer_event_log,
                ("TLS_DNS_PEER:QUERY_SEEN=" + requested_hostname + ",1,1",),
                5.0,
            )
            if "TLS_DNS_PEER:QUERY_RECEIVED" in peer_output:
                raise RuntimeError("NXDOMAIN peer incorrectly returned a DNS answer")
            print("RA4_WIFIS3_TLS_NXDOMAIN_GATE_PASS")
        elif args.expect_handshake_timeout:
            peer_output += wait_for_peer_events(
                peer_event_log,
                (
                    "TLS_DNS_PEER:QUERY_RECEIVED",
                    "TLS_DNS_PEER:HANDSHAKE_STALL_STARTED",
                    "TLS_DNS_PEER:HANDSHAKE_STALL_CLIENT_CLOSED",
                ),
                5.0,
            )
            elapsed_marker = "RA4_TLS_CLIENT:TLS_CONNECT_ELAPSED_MS="
            elapsed_start = capture.find(elapsed_marker)
            elapsed_end = capture.find("\n", elapsed_start)
            elapsed_text = capture[
                elapsed_start + len(elapsed_marker):
                elapsed_end if elapsed_end >= 0 else len(capture)
            ].strip()
            try:
                elapsed_ms = int(elapsed_text)
            except ValueError as error:
                raise RuntimeError(
                    "TLS timeout gate emitted an invalid elapsed marker"
                ) from error
            diagnostic_marker = "RA4_TLS_CLIENT:TLS_DIAG="
            diagnostic_start = capture.find(diagnostic_marker)
            diagnostic_end = capture.find("\n", diagnostic_start)
            diagnostic_text = capture[
                diagnostic_start + len(diagnostic_marker):
                diagnostic_end if diagnostic_end >= 0 else len(capture)
            ].strip()
            try:
                diagnostic_fields = [
                    int(field) for field in diagnostic_text.split(",")
                ]
            except ValueError as error:
                raise RuntimeError(
                    "TLS timeout gate emitted an invalid TLS diagnostic"
                ) from error
            if len(diagnostic_fields) != 7:
                raise RuntimeError(
                    "TLS timeout gate requires phase,error,three heap snapshots,"
                    " requested timeout, and bridge handshake elapsed diagnostics"
                )
            bridge_timeout_ms = diagnostic_fields[5]
            bridge_elapsed_ms = diagnostic_fields[6]
            if bridge_timeout_ms != args.tls_client_timeout_ms:
                raise RuntimeError(
                    "TLS timeout diagnostic did not retain the requested timeout "
                    f"({bridge_timeout_ms} ms vs {args.tls_client_timeout_ms} ms)"
                )
            if (
                bridge_elapsed_ms < args.tls_client_timeout_ms // 2
                or bridge_elapsed_ms > args.tls_client_timeout_ms + 3000
            ):
                raise RuntimeError(
                    "BL616CL TLS handshake timeout did not honor the requested "
                    f"deadline ({bridge_elapsed_ms} ms vs "
                    f"{args.tls_client_timeout_ms} ms)"
                )
            if args.require_ra4_deadline and (
                elapsed_ms < args.tls_client_timeout_ms // 2
                or elapsed_ms > args.tls_client_timeout_ms + 3000
            ):
                raise RuntimeError(
                    "RA4 WiFiSSLClient did not return on the BL616CL "
                    f"deadline ({elapsed_ms} ms vs "
                    f"{args.tls_client_timeout_ms} ms)"
                )
            elapsed_total_ms = int((time.monotonic() - command_started_at) * 1000)
            print(f"RA4_WIFIS3_TLS_TIMEOUT_RA4_ELAPSED_MS={elapsed_ms}")
            print(
                "RA4_WIFIS3_TLS_TIMEOUT_BRIDGE_ELAPSED_MS="
                f"{bridge_elapsed_ms}"
            )
            if args.use_wifis3_result_error_overlay:
                print("RA4_WIFIS3_RESULT_ERROR_OVERLAY_USED")
            if args.require_ra4_deadline:
                print("RA4_WIFIS3_TLS_TIMEOUT_RA4_DEADLINE_PASS")
            print(f"RA4_WIFIS3_TLS_TIMEOUT_TOTAL_MS={elapsed_total_ms}")
            print("RA4_WIFIS3_TLS_HANDSHAKE_TIMEOUT_GATE_PASS")
        else:
            peer_output += wait_for_peer_events(
                peer_event_log,
                (
                    "TLS_DNS_PEER:SNI_PASS=1",
                    "TLS_DNS_PEER:EXCHANGE_PASS",
                ),
                5.0,
            )
            print("RA4_WIFIS3_TLS_GATE_PASS")
    except (MarkerCaptureError, MarkerCaptureTimeoutError) as error:
        print_markers("PAIRED_TLS_CLIENT_DIAGNOSTICS", error.captured)
        primary_error = error
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as error:
        primary_error = error
    finally:
        if bridge is not None and bridge.is_open:
            bridge.close()
        if peer is not None:
            # Preserve peer-side TCP/SNI/alert evidence on client failures.
            if peer.poll() is None:
                try:
                    peer.wait(timeout=3.5)
                except subprocess.TimeoutExpired:
                    pass
            stop_windows_peer(peer, work_dir)
        remove_peer_firewall(firewall_rule)
        peer_output = read_peer_event_log(peer_event_log) or peer_output
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
