#!/usr/bin/env python3
"""Measure verified bidirectional WiFiSSLClient payload throughput on BL616CL."""

from __future__ import annotations

import argparse
import ipaddress
import re
import secrets
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
    wait_for_peer_events,
    windows_wlan_address,
    wsl_unc_path,
)
from bounded_serial_close import close_serial_bounded
from serial_control_write import write_control
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "examples/ra4-wifis3-tls-throughput-gate/ra4-wifis3-tls-throughput-gate.ino.in"
WORK_ROOT = ROOT / "build/ra4-wifis3-tls-throughput-gate"
SAMBA_RUNNER = ROOT / "tools/run_ra4_bossac_final_carrier.py"
TLS_PORT = 18452
TOTAL_BYTES = 64000

WINDOWS_PEER = r"""
import argparse
import os
from pathlib import Path
import socket
import ssl
import time

parser = argparse.ArgumentParser()
parser.add_argument("--bind-ip", required=True)
parser.add_argument("--port", type=int, required=True)
parser.add_argument("--cert", required=True)
parser.add_argument("--key", required=True)
parser.add_argument("--event-log", required=True)
args = parser.parse_args()
Path(__file__).with_suffix(".pid").write_text(str(os.getpid()), encoding="ascii")


def emit(marker):
    with open(args.event_log, "a", encoding="ascii") as events:
        events.write(marker + "\n")


def read_exact(link, size):
    result = bytearray()
    while len(result) < size:
        chunk = link.recv(size - len(result))
        if not chunk:
            raise RuntimeError("TLS peer received a short payload")
        result.extend(chunk)
    return bytes(result)


def valid_payload(payload, sequence):
    return len(payload) == 1000 and int.from_bytes(payload[:4], "big") == sequence and all(
        value == ((sequence * 31 + index * 17) & 0xFF)
        for index, value in enumerate(payload[4:], start=4)
    )


context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.load_cert_chain(args.cert, args.key)
context.minimum_version = ssl.TLSVersion.TLSv1_2
context.maximum_version = ssl.TLSVersion.TLSv1_2
context.set_ciphers("ECDHE-ECDSA-AES128-GCM-SHA256")
with socket.socket() as listener:
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.bind_ip, args.port))
    listener.listen(1)
    listener.settimeout(180)
    emit("TLS_THROUGHPUT_PEER:READY")
    connection, _ = listener.accept()
    connection.settimeout(30)
    with context.wrap_socket(connection, server_side=True) as secure:
        emit("TLS_THROUGHPUT_PEER:TLS_CONNECTED")
        started = time.monotonic()
        for sequence in range(64):
            payload = read_exact(secure, 1000)
            if not valid_payload(payload, sequence):
                emit("TLS_THROUGHPUT_PEER:PAYLOAD_FAIL")
                raise SystemExit(2)
            secure.sendall(payload)
        elapsed_ms = max(1, int((time.monotonic() - started) * 1000))
        emit("TLS_THROUGHPUT_PEER:BYTES_EACH_DIRECTION=64000")
        emit(f"TLS_THROUGHPUT_PEER:ELAPSED_MS={elapsed_ms}")
        emit("TLS_THROUGHPUT_PEER:RESULT=PASS")
"""


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command), flush=True)
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def compile_sketch(work_dir: Path, ca: Path, arduino_cli: str, softap: bool = False) -> Path:
    sketch_dir = work_dir / "ra4-wifis3-tls-throughput-gate"
    sketch_dir.mkdir()
    (sketch_dir / "ra4-wifis3-tls-throughput-gate.ino").write_text(
        TEMPLATE.read_text(encoding="ascii").replace(
            "@TLS_TRUSTED_CA_LITERAL@",
            pem_c_literal(ca.read_text(encoding="ascii")),
        ).replace("@SOFTAP@", "true" if softap else "false"), encoding="ascii",
    )
    build_dir = work_dir / "build"
    run_command([
        arduino_cli, "compile", "--clean", "--fqbn",
        "arduino:renesas_uno:unor4wifi", "--build-path", str(build_dir),
        str(sketch_dir),
    ])
    image = build_dir / "ra4-wifis3-tls-throughput-gate.ino.bin"
    if not image.is_file():
        raise RuntimeError("TLS throughput image is missing")
    return image


def stop_throughput_peer(peer, peer_dir):
    try:
        peer.wait(timeout=1)
        return
    except subprocess.TimeoutExpired:
        pass
    pid_file = peer_dir / "windows_tls_throughput_peer.pid"
    pid = pid_file.read_text(encoding="ascii").strip()
    if not pid.isdigit() or int(pid) < 4:
        raise RuntimeError("invalid owned throughput peer PID")
    # The owned script publishes its own PID; avoid a global CIM process scan.
    result = subprocess.run(["taskkill.exe", "/PID", pid, "/T", "/F"],
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
    if result.returncode and peer.poll() is None:
        raise RuntimeError("owned throughput peer termination failed")
    peer.wait(timeout=5)


def reopen_after_association(link: serial.Serial) -> serial.Serial:
    # WLAN preparation may invalidate the WSL USB mapping and its old fd.
    # Reopen only before START; never reconnect/retry application traffic.
    from bl616cl_usb_mapping import ensure_bl_usb_mapping
    port = ensure_bl_usb_mapping()
    if link is not None:
        close_serial_bounded(link, phase="tls-throughput-association")
    reopened = serial.Serial(port, 115200, timeout=0.1, write_timeout=1)
    reopened.dtr = False
    reopened.rts = False
    print("TLS_THROUGHPUT_ASSOCIATION_USB_IDENTITY_REOPEN_PASS", flush=True)
    return reopened


def wait_marker(link: serial.Serial, marker: str, timeout_seconds: float) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        received.extend(link.read(512))
        if len(received) > 16384:
            raise RuntimeError("TLS throughput preparation capture exceeded limit")
        output = received.decode("ascii", errors="replace").replace("\r", "")
        if "RA4_TLS_THROUGHPUT:RESULT=FAIL:" in output:
            raise RuntimeError("TLS throughput preparation failed")
        if marker in output:
            return output
    raise RuntimeError("TLS throughput preparation timed out")


def query_preparation(link):
    nonce = secrets.token_hex(8)
    payload = f"@PREPSTATUS {nonce}\n".encode("ascii")
    write_control(link, payload, phase="tls-throughput-preparation-query")
    marker = "RA4_TLS_THROUGHPUT:PREPSTATUS=" + nonce + ",1"
    captured = wait_marker(link, marker, 5)
    if captured.replace("\r", "").splitlines().count(marker) != 1:
        raise RuntimeError("missing unique throughput preparation response")
    print("TLS_THROUGHPUT_CACHED_PREPARATION_QUERY_PASS", flush=True)


def snapshot_usb_control(phase):
    # One reader, sequential pages; no raw CDC data or credentials recorded.
    inspector = ROOT / "tools/inspect_bl616cl_gpio35.py"
    print(f"TLS_THROUGHPUT_USB_SNAPSHOT_BEGIN={phase}", flush=True)
    for options in ([], ["--cdc-in"]):
        try:
            result = subprocess.run([sys.executable, str(inspector), *options],
                                    capture_output=True, text=True, timeout=3, check=True)
            print(result.stdout, end="", flush=True)
        except (OSError, subprocess.SubprocessError) as error:
            print(f"TLS_THROUGHPUT_USB_SNAPSHOT_UNAVAILABLE={phase},type:{type(error).__name__}", flush=True)
    print(f"TLS_THROUGHPUT_USB_SNAPSHOT_END={phase}", flush=True)


def capture(link: serial.Serial, timeout_seconds: float) -> tuple[str, int]:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        received.extend(link.read(512))
        if len(received) > 16384:
            raise RuntimeError("TLS throughput serial capture exceeded limit")
        text = received.decode("ascii", errors="replace").replace("\x00", "").replace("\r", "")
        if "RA4_TLS_THROUGHPUT:RESULT=FAIL:" in text:
            raise RuntimeError("RA4M1 TLS throughput: " + text.split("RESULT=FAIL:")[-1].splitlines()[0])
        if "RA4_TLS_THROUGHPUT:RESULT=PASS" in text:
            required = (
                "RA4_TLS_THROUGHPUT:WIFI_CONNECTED",
                "RA4_TLS_THROUGHPUT:TLS_CONNECTED",
                "RA4_TLS_THROUGHPUT:CHUNKS=64",
                "RA4_TLS_THROUGHPUT:BYTES_EACH_DIRECTION=64000",
            )
            if any(marker not in text for marker in required):
                raise RuntimeError("RA4M1 TLS throughput result is incomplete")
            match = re.search(r"^RA4_TLS_THROUGHPUT:ELAPSED_MS=(\d+)$", text, re.M)
            if match is None or int(match[1]) <= 0:
                raise RuntimeError("RA4M1 TLS elapsed time is invalid")
            return text, int(match[1])
    raise RuntimeError("RA4M1 TLS throughput timed out")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--skip-peer-firewall", action="store_true")
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--usb-control-diagnostics", action="store_true",
                        help="Read numeric HID counters before START and after failure; requires exclusive HID ownership.")
    parser.add_argument("--softap", action="store_true",
                        help="Use the board AP and a temporary Windows WLAN profile; no upstream AP needed.")
    parser.add_argument("--runtime-settle-ms", type=int, default=1000)
    args = parser.parse_args()
    WORK_ROOT.mkdir(parents=True, exist_ok=True)
    work_dir = Path(tempfile.mkdtemp(prefix="run-", dir=WORK_ROOT))
    peer_dir = work_dir / "peer"
    peer_dir.mkdir()
    peer = None
    bridge = None
    firewall = None
    ra4_modified = False
    error: Exception | None = None
    events = work_dir / "peer.events"
    softap_session = None
    detach_helper = None
    try:
        if args.softap and not args.source_only:
            from bl616cl_usb_mapping import ensure_bl_usb_mapping
            from run_ra4_wifis3_tls_resource_gate import start_preparation_detach
            args.bridge_port = ensure_bl_usb_mapping()
            detach_helper = start_preparation_detach(args)
        host = ipaddress.IPv4Address("192.168.4.100" if args.softap else
                                   "192.0.2.1" if args.source_only else windows_wlan_address())
        ca, certificate, key = create_material(work_dir, "ec", host)
        image = compile_sketch(work_dir, ca, args.arduino_cli, args.softap)
        if args.source_only:
            print("RA4_WIFIS3_TLS_THROUGHPUT_SOURCE_COMPILE_PASS")
            return 0

        args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
        final_scan.require_final_carrier_bridge(args.bridge_port)
        if args.softap:
            ssid, password = "uno-r4-" + secrets.token_hex(4), secrets.token_urlsafe(18)
        else:
            ssid, password = load_private_credentials(args.credential_env)
        if password is None:
            raise RuntimeError("private WPA password required")
        print(f"Bridge: {args.bridge_port} (verified BL616CL final carrier)")
        print("Target: RA4M1 application Flash only; BL616CL Flash unchanged.")
        print("TLS_THROUGHPUT_LINK=" + ("DIRECT_SOFTAP" if args.softap else "UPSTREAM_STA"))
        # The certificate fixes the peer address before association. Check host
        # permission before replacing the Sketch or changing Windows WLAN.
        if not args.skip_peer_firewall:
            firewall = configure_peer_firewall(str(host), TLS_PORT)
        # Upload before starting the peer: in AP mode its DHCP address is checked
        # after the final Sketch reset, against the IP in the generated certificate.
        ra4_modified = True
        run_command([
            sys.executable, str(SAMBA_RUNNER), "--bridge-port", args.bridge_port,
            "--image", str(image), "--runtime-settle-ms", str(args.runtime_settle_ms), "--attempts", "3",
        ])
        bridge = serial.Serial(args.bridge_port, 115200, timeout=0.1, write_timeout=1)
        bridge.dtr = False
        bridge.rts = False
        wait_marker(bridge, "RA4_TLS_THROUGHPUT:READY", 15)
        bridge.reset_input_buffer()
        if args.softap:
            from bl616cl_softap_test_session import SoftApTestSession
            softap_session = SoftApTestSession(work_dir)
            softap_session.prepare_association(args, ssid, password, "192.168.4.1")
            write_control(bridge, (f"ARMPAP {encode_start_value(ssid)} {encode_start_value(password)} "
                          f"{host} {TLS_PORT}\n").encode("ascii"), phase="tls-throughput-arm")
            wait_marker(bridge, "RA4_TLS_THROUGHPUT:SOFTAP_ARMED", 5)
            from run_ra4_wifis3_tls_resource_gate import suspend_before_association
            suspend_before_association(args, bridge, detach_helper)
            bridge = None
            print("TLS_THROUGHPUT_SOFTAP_ARM_SETTLE_BEGIN", flush=True)
            time.sleep(10)
            softap_session.connect_prepared()
            if ipaddress.IPv4Address(softap_session.current_address()) != host:
                raise RuntimeError("SoftAP DHCP lease differs from TLS certificate address")
            bridge = reopen_after_association(bridge)
            time.sleep(1)
            query_preparation(bridge)
        # Scope the forced Windows cleanup to the TLS peer, excluding the WLAN
        # profile owner whose finally block must restore the host network.
        script = peer_dir / "windows_tls_throughput_peer.py"
        script.write_text(textwrap.dedent(WINDOWS_PEER), encoding="ascii")
        peer = subprocess.Popen([
            "powershell.exe", "-NoProfile", "-Command",
            f'& py.exe -3 "{wsl_unc_path(script)}" --bind-ip {host} --port {TLS_PORT} '
            f'--cert "{wsl_unc_path(certificate)}" --key "{wsl_unc_path(key)}" '
            f'--event-log "{wsl_unc_path(events)}"',
        ], stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
        wait_for_peer_events(events, ("TLS_THROUGHPUT_PEER:READY",), 15)
        if args.usb_control_diagnostics:
            snapshot_usb_control("before-start")
        bridge.reset_input_buffer()
        write_control(bridge, (
            f"START {encode_start_value(ssid)} {encode_start_value(password)} "
            f"{host} {TLS_PORT}\n"
        ).encode("ascii"), phase="tls-throughput-start")
        output, elapsed_ms = capture(bridge, 180)
        peer_output = wait_for_peer_events(events, (
            "TLS_THROUGHPUT_PEER:TLS_CONNECTED",
            "TLS_THROUGHPUT_PEER:BYTES_EACH_DIRECTION=64000",
            "TLS_THROUGHPUT_PEER:RESULT=PASS",
        ), 15)
        for line in output.splitlines():
            if line.startswith("RA4_TLS_THROUGHPUT:"):
                print(line)
        for line in peer_output.splitlines():
            if line.startswith("TLS_THROUGHPUT_PEER:"):
                print(line)
        print(f"RA4_TLS_THROUGHPUT:ONE_WAY_GOODPUT_BPS={TOTAL_BYTES * 1000 // elapsed_ms}")
        print(f"RA4_TLS_THROUGHPUT:ROUNDTRIP_PAYLOAD_BPS={TOTAL_BYTES * 2000 // elapsed_ms}")
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as caught:
        error = caught
        if args.usb_control_diagnostics and bridge is not None:
            snapshot_usb_control("failure")
    finally:
        print("TLS_THROUGHPUT_CLEANUP_BEGIN", flush=True)
        if detach_helper is not None and detach_helper.poll() is None:
            detach_helper.terminate()
            try:
                detach_helper.wait(timeout=5)
            except subprocess.SubprocessError as cleanup_error:
                if error is None:
                    error = cleanup_error
        if bridge is not None and bridge.is_open:
            try:
                close_serial_bounded(bridge, phase="tls-throughput-cleanup")
            except RuntimeError as cleanup_error:
                if error is None:
                    error = cleanup_error
            finally:
                bridge = None
        if peer is not None:
            try:
                stop_throughput_peer(peer, peer_dir)
            except (OSError, RuntimeError, subprocess.SubprocessError) as cleanup_error:
                if error is None:
                    error = cleanup_error
        remove_peer_firewall(firewall)
        if softap_session is not None:
            try:
                print("TLS_THROUGHPUT_NETWORK_RESTORE_BEGIN", flush=True)
                softap_session.close()
                print("TLS_THROUGHPUT_NETWORK_RESTORE_PASS", flush=True)
            except (OSError, RuntimeError, subprocess.SubprocessError) as cleanup_error:
                if error is None:
                    error = cleanup_error
        if ra4_modified:
            try:
                if softap_session is not None:
                    from bl616cl_usb_mapping import ensure_bl_usb_mapping
                    args.bridge_port = ensure_bl_usb_mapping()
                run_command([
                    sys.executable, str(final_scan.__file__), "--bridge-port", args.bridge_port,
                    "--arduino-cli", args.arduino_cli,
                ])
                print("RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_PASS")
            except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as restore_error:
                print("RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_FAILED", file=sys.stderr)
                if error is None:
                    error = restore_error
        if error is None:
            shutil.rmtree(work_dir, ignore_errors=True)
        else:
            print(f"TLS_THROUGHPUT_FAILURE_EVIDENCE={work_dir}", file=sys.stderr)
    if error is not None:
        raise error
    print("RA4_WIFIS3_TLS_THROUGHPUT_GATE_PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException, subprocess.SubprocessError) as error:
        print(f"ERROR: {error}", file=sys.stderr)
        raise SystemExit(1)
