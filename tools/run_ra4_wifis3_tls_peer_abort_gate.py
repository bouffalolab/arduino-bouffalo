#!/usr/bin/env python3
"""Run TLS peer-RST detection and reconnect on the final BL616CL carrier."""

from __future__ import annotations

import argparse
import ipaddress
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
    encode_start_value,
    pem_c_literal,
    read_peer_event_log,
    remove_peer_firewall,
    stop_windows_peer,
    wait_for_peer_events,
    windows_wlan_address,
    wsl_unc_path,
)
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT = Path(__file__).resolve().parents[1]
FQBN = "arduino:renesas_uno:unor4wifi"
TEMPLATE = (
    ROOT
    / "examples"
    / "ra4-wifis3-tls-peer-abort-gate"
    / "ra4-wifis3-tls-peer-abort-gate.ino.in"
)
WORK_ROOT = ROOT / "build" / "ra4-wifis3-tls-peer-abort-gate"
OVERLAY_PREPARER = ROOT / "tools" / "prepare_wifis3_result_error_overlay.py"
SAMBA_RUNNER = ROOT / "tools" / "run_ra4_bossac_final_carrier.py"
TLS_PORT = 18451


WINDOWS_PEER = r"""
import argparse
import socket
import ssl
import struct

parser = argparse.ArgumentParser()
parser.add_argument("--bind-ip", required=True)
parser.add_argument("--port", required=True, type=int)
parser.add_argument("--cert", required=True)
parser.add_argument("--key", required=True)
parser.add_argument("--event-log", required=True)
args = parser.parse_args()
expected = bytes((0x60 + index) & 0xFF for index in range(32))


def emit(message):
    print(message, flush=True)
    with open(args.event_log, "a", encoding="ascii") as events:
        events.write(message + "\n")


def recv_exact(connection, size):
    data = bytearray()
    while len(data) < size:
        part = connection.recv(size - len(data))
        if not part:
            return bytes(data)
        data.extend(part)
    return bytes(data)


context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(certfile=args.cert, keyfile=args.key)
context.minimum_version = ssl.TLSVersion.TLSv1_2
context.maximum_version = ssl.TLSVersion.TLSv1_2
context.set_ciphers("ECDHE-ECDSA-AES128-GCM-SHA256")

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.bind_ip, args.port))
    listener.listen(2)
    listener.settimeout(120)
    emit("TLS_PEER_ABORT:READY")

    first, _ = listener.accept()
    first.settimeout(20)
    first_tls = context.wrap_socket(first, server_side=True)
    emit("TLS_PEER_ABORT:FIRST_HANDSHAKE_PASS")
    first_tls.setsockopt(
        socket.SOL_SOCKET,
        socket.SO_LINGER,
        struct.pack("ii", 1, 0),
    )
    first_tls.close()
    emit("TLS_PEER_ABORT:TCP_RST_SENT")

    second, _ = listener.accept()
    second.settimeout(20)
    with context.wrap_socket(second, server_side=True) as second_tls:
        emit("TLS_PEER_ABORT:SECOND_HANDSHAKE_PASS")
        payload = recv_exact(second_tls, len(expected))
        if payload != expected:
            emit("TLS_PEER_ABORT:RECOVERY_PAYLOAD_FAIL")
            raise SystemExit(2)
        second_tls.sendall(payload)
        emit("TLS_PEER_ABORT:RECOVERY_ECHO_PASS")

emit("TLS_PEER_ABORT:RESULT=PASS")
"""


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def compile_sketch(
    args: argparse.Namespace,
    work_dir: Path,
    trusted_ca: Path,
) -> Path:
    sketch_dir = work_dir / "ra4-wifis3-tls-peer-abort-gate"
    sketch_dir.mkdir(parents=True, exist_ok=True)
    sketch = sketch_dir / "ra4-wifis3-tls-peer-abort-gate.ino"
    sketch.write_text(
        TEMPLATE.read_text(encoding="ascii").replace(
            "@TLS_TRUSTED_CA_LITERAL@",
            pem_c_literal(trusted_ca.read_text(encoding="ascii")),
        ),
        encoding="ascii",
    )
    overlay_library = work_dir / "libraries" / "WiFiS3"
    run_command([
        sys.executable,
        str(OVERLAY_PREPARER),
        "--output-library",
        str(overlay_library),
    ])
    build_dir = work_dir / "build"
    run_command([
        args.arduino_cli,
        "compile",
        "--clean",
        "--fqbn",
        FQBN,
        "--build-path",
        str(build_dir),
        "--library",
        str(overlay_library),
        str(sketch_dir),
    ])
    image = build_dir / "ra4-wifis3-tls-peer-abort-gate.ino.bin"
    if not image.is_file():
        raise RuntimeError("compiled RA4M1 TLS peer-abort image is missing")
    return image


def wait_ready(link: serial.Serial, timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        received.extend(link.read(256))
        if b"RA4_TLS_PEER_ABORT:READY" in received:
            return
        if len(received) > 4096:
            del received[:-1024]
    raise RuntimeError("RA4M1 TLS peer-abort Sketch did not start")


def capture_result(link: serial.Serial, timeout_seconds: float) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        received.extend(link.read(512))
        text = (
            received.decode("ascii", errors="replace")
            .replace("\x00", "")
            .replace("\r", "")
        )
        if "RA4_TLS_PEER_ABORT:RESULT=FAIL:" in text:
            print_markers("RA4_TLS_PEER_ABORT_DIAGNOSTICS", text)
            raise RuntimeError("RA4M1 TLS peer-abort gate failed")
        if "RA4_TLS_PEER_ABORT:RESULT=PASS" in text:
            for marker in (
                "RA4_TLS_PEER_ABORT:CONNECTED_AFTER_ABORT=0",
                "RA4_TLS_PEER_ABORT:WRITE_AFTER_ABORT=0",
                "RA4_TLS_PEER_ABORT:ABORT_DETECTED_PASS",
                "RA4_TLS_PEER_ABORT:CLOSE_PASS",
                "RA4_TLS_PEER_ABORT:RECOVERY_HANDSHAKE_PASS",
                "RA4_TLS_PEER_ABORT:RECOVERY_ECHO_PASS",
            ):
                if marker not in text:
                    raise RuntimeError(
                        f"TLS peer-abort result is missing {marker}"
                    )
            return text
    print_markers("RA4_TLS_PEER_ABORT_TIMEOUT", received.decode(
        "ascii", errors="replace"))
    raise RuntimeError("RA4M1 TLS peer-abort gate timed out")


def print_markers(label: str, text: str) -> None:
    markers = [
        line.strip()
        for line in text.replace("\x00", "").splitlines()
        if line.strip().startswith("RA4_TLS_PEER_ABORT:")
    ]
    if markers:
        print(label + "_BEGIN")
        for marker in markers:
            print(marker)
        print(label + "_END")


def launch_peer(
    work_dir: Path,
    host: ipaddress.IPv4Address,
    certificate: Path,
    key: Path,
    event_log: Path,
) -> subprocess.Popen[str]:
    script = work_dir / "windows_tls_peer_abort.py"
    script.write_text(textwrap.dedent(WINDOWS_PEER), encoding="ascii")
    return subprocess.Popen(
        [
            "powershell.exe",
            "-NoProfile",
            "-Command",
            (
                f'& py.exe -3 "{wsl_unc_path(script)}" '
                f"--bind-ip {host} --port {TLS_PORT} "
                f'--cert "{wsl_unc_path(certificate)}" '
                f'--key "{wsl_unc_path(key)}" '
                f'--event-log "{wsl_unc_path(event_log)}"'
            ),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def restore_scan(args: argparse.Namespace) -> None:
    run_command([
        sys.executable,
        str(final_scan.__file__),
        "--bridge-port",
        args.bridge_port,
        "--arduino-cli",
        args.arduino_cli,
    ])
    print("RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_PASS")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--skip-peer-firewall", action="store_true")
    parser.add_argument("--source-only", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    WORK_ROOT.mkdir(parents=True, exist_ok=True)
    work_dir = Path(tempfile.mkdtemp(prefix="run-", dir=WORK_ROOT))
    peer = None
    bridge = None
    firewall_rule = None
    ra4_modified = False
    primary_error: Exception | None = None
    events = work_dir / "peer.events"
    try:
        material_dir = work_dir / "material"
        material_dir.mkdir()
        material_host = ipaddress.IPv4Address("192.0.2.1")
        if not args.source_only:
            material_host = ipaddress.IPv4Address(windows_wlan_address())
        ca, certificate, key = create_material(
            material_dir,
            "ec",
            material_host,
        )
        image = compile_sketch(args, work_dir, ca)
        if args.source_only:
            print("RA4_WIFIS3_TLS_PEER_ABORT_SOURCE_COMPILE_PASS")
            return 0

        args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
        final_scan.require_final_carrier_bridge(args.bridge_port)
        ssid, password = load_private_credentials(args.credential_env)
        if password is None:
            raise RuntimeError("TLS peer-abort gate requires a private WPA password")
        host = material_host
        print(
            f"BL616CL final carrier: {args.bridge_port} "
            "(2341:1002 interface 00)"
        )
        print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")
        if not args.skip_peer_firewall:
            firewall_rule = configure_peer_firewall(str(host), TLS_PORT)
        peer = launch_peer(work_dir, host, certificate, key, events)
        wait_for_peer_events(events, ("TLS_PEER_ABORT:READY",), 15.0)

        run_command([
            sys.executable,
            str(SAMBA_RUNNER),
            "--bridge-port",
            args.bridge_port,
            "--image",
            str(image),
            "--runtime-settle-ms",
            "1000",
            "--attempts",
            "3",
        ])
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
        wait_ready(bridge, 15.0)
        bridge.reset_input_buffer()
        bridge.write(
            (
                f"START {encode_start_value(ssid)} "
                f"{encode_start_value(password)} {host} {TLS_PORT}\n"
            ).encode("ascii")
        )
        bridge.flush()
        output = capture_result(bridge, 90.0)
        print_markers("RA4_TLS_PEER_ABORT_RUNTIME", output)
        peer_output = wait_for_peer_events(
            events,
            (
                "TLS_PEER_ABORT:FIRST_HANDSHAKE_PASS",
                "TLS_PEER_ABORT:TCP_RST_SENT",
                "TLS_PEER_ABORT:SECOND_HANDSHAKE_PASS",
                "TLS_PEER_ABORT:RECOVERY_ECHO_PASS",
                "TLS_PEER_ABORT:RESULT=PASS",
            ),
            10.0,
        )
        for line in peer_output.splitlines():
            if line.startswith("TLS_PEER_ABORT:"):
                print(line)
        print("RA4_WIFIS3_TLS_PEER_ABORT_GATE_PASS")
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
            stop_windows_peer(peer, work_dir)
        remove_peer_firewall(firewall_rule)
        peer_output = read_peer_event_log(events)
        if primary_error is not None:
            for line in peer_output.splitlines():
                if line.startswith("TLS_PEER_ABORT:"):
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
                print("RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_FAILED", file=sys.stderr)
                if primary_error is None:
                    primary_error = RuntimeError(
                        "final-carrier RA4M1 scan restore failed"
                    )
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
