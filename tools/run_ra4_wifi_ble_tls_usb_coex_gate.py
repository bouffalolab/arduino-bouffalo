#!/usr/bin/env python3
"""Run RA4M1 Wi-Fi + ArduinoBLE scan + TLS + USB CDC coexistence on BL616CL."""

from __future__ import annotations

import argparse
import re
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
import run_ra4_wifis3_tls_gate as tls_gate
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT_DIR = Path(__file__).resolve().parents[1]
FQBN = "arduino:renesas_uno:unor4wifi"
TEMPLATE = (
    ROOT_DIR
    / "examples"
    / "ra4-wifi-ble-tls-usb-coex"
    / "ra4-wifi-ble-tls-usb-coex.ino.in"
)
WORK_ROOT = ROOT_DIR / "build" / "ra4-wifi-ble-tls-usb-coex"
SAMBA_RUNNER = ROOT_DIR / "tools" / "run_ra4_bossac_final_carrier.py"
WIFIS3_OVERLAY_PREPARER = (
    ROOT_DIR / "tools" / "prepare_wifis3_result_error_overlay.py"
)
TLS_PORT = 18443

WINDOWS_COEX_PEER = r"""
import argparse
import re
import socket
import ssl

EXPECTED_REQUEST = b"bl616cl-tls-ping"
EXPECTED_REPLY = b"bl616cl-tls-echo"


def emit(message, event_log):
    print(message, flush=True)
    with open(event_log, "a", encoding="ascii") as output:
        output.write(message + "\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--bind-ip", required=True)
    parser.add_argument("--port", required=True, type=int)
    parser.add_argument("--cert", required=True)
    parser.add_argument("--key", required=True)
    parser.add_argument("--expected-sni", required=True)
    parser.add_argument("--event-log", required=True)
    parser.add_argument("--rounds", required=True, type=int)
    parser.add_argument("--handshake-timeout", required=True, type=float)
    args = parser.parse_args()
    if args.rounds <= 0:
        raise RuntimeError("round count must be positive")

    sni = {"name": "", "ok": False}
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(certfile=args.cert, keyfile=args.key)
    context.minimum_version = ssl.TLSVersion.TLSv1_2
    context.maximum_version = ssl.TLSVersion.TLSv1_2
    context.set_ciphers("ECDHE-ECDSA-AES128-GCM-SHA256")

    def on_sni(_socket, server_name, _context):
        sni["name"] = server_name or ""
        sni["ok"] = sni["name"] == args.expected_sni

    context.set_servername_callback(on_sni)
    listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.bind_ip, args.port))
    listener.listen(8)
    listener.settimeout(0.2)
    completed = 0
    accepted = 0
    emit("TLS_DNS_PEER:READY", args.event_log)
    try:
        while completed < args.rounds:
            try:
                connection, _ = listener.accept()
            except socket.timeout:
                continue
            accepted += 1
            emit(f"TLS_DNS_PEER:TCP_ACCEPTED={accepted}", args.event_log)
            with connection:
                try:
                    connection.settimeout(args.handshake_timeout)
                    sni["name"] = ""
                    sni["ok"] = False
                    emit("TLS_DNS_PEER:HANDSHAKE_STARTED", args.event_log)
                    with context.wrap_socket(connection, server_side=True) as secure:
                        cipher = secure.cipher()
                        emit(
                            "TLS_DNS_PEER:NEGOTIATED="
                            + str(secure.version())
                            + ","
                            + str(cipher[0] if cipher else "none"),
                            args.event_log,
                        )
                        emit(
                            "TLS_DNS_PEER:SNI="
                            + (sni["name"] or "<missing>"),
                            args.event_log,
                        )
                        emit(
                            "TLS_DNS_PEER:SNI_PASS="
                            + ("1" if sni["ok"] else "0"),
                            args.event_log,
                        )
                        request = secure.recv(128)
                        if sni["ok"] and request == EXPECTED_REQUEST:
                            secure.sendall(EXPECTED_REPLY)
                            completed += 1
                            emit(
                                f"TLS_DNS_PEER:EXCHANGE_PASS={completed}",
                                args.event_log,
                            )
                        else:
                            emit("TLS_DNS_PEER:EXCHANGE_REJECTED", args.event_log)
                except ssl.SSLError as error:
                    reason = getattr(error, "reason", None) or "unknown"
                    emit(f"TLS_DNS_PEER:HANDSHAKE_ERROR={reason}", args.event_log)
                    emit("TLS_DNS_PEER:HANDSHAKE_REJECTED", args.event_log)
                except OSError as error:
                    emit(
                        "TLS_DNS_PEER:TCP_ERROR=" + type(error).__name__,
                        args.event_log,
                    )
    finally:
        listener.close()


if __name__ == "__main__":
    try:
        main()
    except Exception as error:
        # Paths, peer addresses, and PKI contents stay out of startup errors.
        code = getattr(error, "winerror", None) or getattr(error, "errno", None) or 0
        print("TLS_DNS_PEER:STARTUP_ERROR=" + type(error).__name__ + "," + str(code),
              flush=True)
        raise SystemExit(1)
"""


class MarkerCaptureError(RuntimeError):
    def __init__(self, marker: str, captured: str) -> None:
        super().__init__(f"RA4M1 coexistence gate failed: {marker}")
        self.captured = captured


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def encode_start_value(value: str) -> str:
    return value.encode("ascii", "strict").hex()


def print_markers(label: str, text: str) -> None:
    markers = [
        line.strip()
        for line in text.replace("\x00", "").splitlines()
        if line.strip().startswith("RA4_COEX:")
    ]
    if markers:
        print(label + "_BEGIN")
        for marker in markers:
            print(marker)
        print(label + "_END")


def capture_until(
    link: serial.Serial,
    required: tuple[str, ...],
    timeout_seconds: float,
    send_echo: bool,
    expected_echoes: int,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    echo_sent = 0
    while time.monotonic() < deadline:
        payload = link.read(512)
        if not payload:
            continue
        received.extend(payload)
        if len(received) > 32768:
            raise RuntimeError("RA4M1 coexistence output exceeded its byte limit")
        text = received.decode("ascii", errors="replace").replace("\x00", "")
        failure = "RA4_COEX:RESULT=FAIL:"
        if failure in text:
            start = text.find(failure)
            end = text.find("\n", start)
            raise MarkerCaptureError(
                text[start:end if end >= 0 else len(text)].strip(),
                text,
            )
        echo_requests = text.count("RA4_COEX:USB_ECHO_REQUEST")
        while send_echo and echo_sent < echo_requests:
            link.write(b"ECHO\n")
            link.flush()
            echo_sent += 1
        if all(marker in text for marker in required):
            if send_echo and echo_sent != expected_echoes:
                raise RuntimeError("coexistence gate completed without every USB echo request")
            return text
    raise MarkerCaptureError("RA4_COEX:MARKERS_TIMEOUT", received.decode(
        "ascii", errors="replace").replace("\x00", ""))

def launch_windows_coex_peer(
    work_dir: Path,
    host: str,
    tls_port: int,
    certificate: Path,
    key: Path,
    rounds: int,
    event_log: Path,
    handshake_timeout: float,
) -> subprocess.Popen[str]:
    script = work_dir / "windows_coex_tls_peer.py"
    script.write_text(textwrap.dedent(WINDOWS_COEX_PEER), encoding="ascii")
    return subprocess.Popen(
        [
            "powershell.exe",
            "-NoProfile",
            "-Command",
            (
                f'& py.exe -3 "{tls_gate.wsl_unc_path(script)}" '
                f'--bind-ip {host} --port {tls_port} '
                f'--expected-sni {host} --rounds {rounds} '
                f'--handshake-timeout {handshake_timeout} '
                f'--cert "{tls_gate.wsl_unc_path(certificate)}" '
                f'--key "{tls_gate.wsl_unc_path(key)}" '
                f'--event-log "{tls_gate.wsl_unc_path(event_log)}"'
            ),
        ],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
    )


def compile_sketch(
    args: argparse.Namespace,
    work_dir: Path,
    certificate_authority: Path,
) -> Path:
    sketch_dir = work_dir / "ra4-wifi-ble-tls-usb-coex"
    sketch_dir.mkdir(parents=True, exist_ok=True)
    sketch = sketch_dir / "ra4-wifi-ble-tls-usb-coex.ino"
    sketch.write_text(
        TEMPLATE.read_text(encoding="ascii").replace(
            "@TLS_TEST_CA_LITERAL@",
            tls_gate.pem_c_literal(certificate_authority.read_text(encoding="ascii")),
        ).replace("@ENABLE_TCP_PROBE@", "false" if args.skip_tcp_probe else "true"
        ).replace("@TLS_TIMEOUT_MS@", str(args.tls_timeout_ms)
        ).replace("@REDUCED_SCAN_DUTY@", "true" if args.reduced_scan_duty else "false"
        ).replace("@SCAN_WINDOW@", str(args.scan_window)
        ).replace("@SCAN_STOP_RECOVERY@", "true" if args.scan_stop_recovery else "false"),
        encoding="ascii",
    )
    overlay_library = work_dir / "libraries" / "WiFiS3"
    run_command(
        [
            sys.executable,
            str(WIFIS3_OVERLAY_PREPARER),
            "--output-library",
            str(overlay_library),
        ]
    )
    build_dir = work_dir / "build"
    run_command(
        [
            args.arduino_cli,
            "compile",
            "--fqbn",
            FQBN,
            "--libraries",
            str(ROOT_DIR / "libraries"),
            "--library",
            str(overlay_library),
            "--build-path",
            str(build_dir),
            str(sketch_dir),
        ]
    )
    image = build_dir / "ra4-wifi-ble-tls-usb-coex.ino.bin"
    if not image.is_file():
        raise RuntimeError("compiled RA4M1 coexistence image does not exist")
    return image


def upload_sketch(args: argparse.Namespace, image: Path) -> None:
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
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--tls-port", type=int, default=TLS_PORT)
    parser.add_argument("--tls-timeout-ms", type=int, default=10000)
    parser.add_argument("--runtime-settle-ms", type=int, default=2000)
    parser.add_argument("--ready-timeout-ms", type=int, default=15000)
    parser.add_argument("--gate-timeout-ms", type=int, default=300000)
    parser.add_argument(
        "--rounds",
        type=int,
        default=1,
        help="Repeated active-scan/TLS/USB cycles in one RA4M1 runtime (1-20).",
    )
    parser.add_argument("--skip-peer-firewall", action="store_true")
    parser.add_argument("--stop-scan-before-tls", action="store_true")
    parser.add_argument("--scan-stop-recovery", action="store_true",
                        help="diagnose failures after stopping scan; original gate still fails")
    parser.add_argument("--skip-tcp-probe", action="store_true")
    parser.add_argument("--reduced-scan-duty", action="store_true")
    parser.add_argument("--scan-window", type=lambda value: int(value, 0), default=0x12)
    parser.add_argument("--source-only", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    if (
        args.tls_port <= 0
        or args.tls_port > 65535
        or args.tls_timeout_ms < 1000
        or args.tls_timeout_ms > 60000
        or args.runtime_settle_ms < 0
        or args.ready_timeout_ms <= 0
        or args.gate_timeout_ms <= 0
        or args.rounds <= 0
        or args.rounds > 20
        or args.scan_window < 0x11
        or args.scan_window > 0x40
        or (args.scan_window != 0x12 and not args.reduced_scan_duty)
    ):
        raise RuntimeError("invalid timeout")

    WORK_ROOT.mkdir(parents=True, exist_ok=True)
    work_dir = Path(tempfile.mkdtemp(prefix="unor4-coex-", dir=WORK_ROOT))
    if args.source_only:
        try:
            certificate_authority, _, _ = tls_gate.create_material(
                work_dir,
                "ec",
                ipaddress.IPv4Address("192.0.2.1"),
            )
            compile_sketch(args, work_dir, certificate_authority)
            print("RA4_WIFI_BLE_TLS_USB_COEX_SOURCE_COMPILE_PASS")
            return 0
        finally:
            shutil.rmtree(work_dir, ignore_errors=True)

    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    final_scan.require_final_carrier_bridge(args.bridge_port)
    ssid, password = load_private_credentials(args.credential_env)
    if password is None:
        raise RuntimeError("coexistence gate requires a private WPA password")
    host = tls_gate.windows_wlan_address()

    print(f"BL616CL final carrier: {args.bridge_port} (2341:1002 interface 00)")
    print("Target: RA4M1 application Flash only; BL616CL Flash is untouched.")
    print("Privacy: STA credentials, temporary PKI, peer address, and payload stay out of logs.")

    bridge: serial.Serial | None = None
    peer: subprocess.Popen[str] | None = None
    firewall_rule: str | None = None
    peer_event_log = work_dir / "windows_tls_dns_peer.events"
    peer_output = ""
    ra4_modified = False
    primary_error: Exception | None = None
    try:
        certificate_authority, certificate, key = tls_gate.create_material(
            work_dir,
            "ec",
            ipaddress.IPv4Address(host),
        )
        image = compile_sketch(args, work_dir, certificate_authority)
        if args.skip_peer_firewall:
            print("COEX_TLS_PEER:FIREWALL_SKIPPED")
        else:
            firewall_rule = tls_gate.configure_peer_firewall(host, args.tls_port)
        peer = launch_windows_coex_peer(
            work_dir,
            host,
            args.tls_port,
            certificate,
            key,
            args.rounds,
            peer_event_log,
            max(15.0, args.tls_timeout_ms / 1000.0 + 5.0),
        )
        peer_output = tls_gate.wait_for_peer_ready(
            peer, args.ready_timeout_ms / 1000.0
        )

        ra4_modified = True
        upload_sketch(args, image)
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
            ("RA4_COEX:READY",),
            args.ready_timeout_ms / 1000.0,
            send_echo=False,
            expected_echoes=0,
        )
        print_markers("RA4_COEX_STARTUP", startup)
        bridge.reset_input_buffer()
        start = (
            "START "
            + encode_start_value(ssid)
            + " "
            + encode_start_value(password)
            + " "
            + host
            + " "
            + str(args.tls_port)
            + " "
            + ("1" if args.stop_scan_before_tls else "0")
            + " "
            + str(args.rounds)
            + "\n"
        )
        bridge.write(start.encode("ascii"))
        bridge.flush()
        scan_phase_marker = (
            "RA4_COEX:BLE_SCAN_STOP_BEFORE_TLS"
            if args.stop_scan_before_tls
            else "RA4_COEX:BLE_SCAN_ACTIVE_DURING_TLS"
        )
        capture = capture_until(
            bridge,
            (
                "RA4_COEX:WIFI_CONNECTED",
                "RA4_COEX:BLE_BEGIN_PASS",
                "RA4_COEX:BLE_SCAN_START_PASS",
                "RA4_COEX:BLE_SCAN_PARAMETERS_CUSTOM"
                if args.reduced_scan_duty else "RA4_COEX:BLE_SCAN_START_PASS",
                f"RA4_COEX:BLE_SCAN_WINDOW={args.scan_window}"
                if args.reduced_scan_duty else "RA4_COEX:BLE_SCAN_START_PASS",
                "RA4_COEX:BLE_REPORTS_BEFORE_TLS=",
                "RA4_COEX:TCP_PROBE_SKIPPED"
                if args.skip_tcp_probe else "RA4_COEX:TCP_PROBE_PASS",
                scan_phase_marker,
                "RA4_COEX:TLS_CONNECT_PASS",
                "RA4_COEX:TLS_WRITE_PASS",
                "RA4_COEX:TLS_READ_PASS",
                "RA4_COEX:USB_ECHO_PASS",
                "RA4_COEX:TLS_CLOSE_PASS",
                "RA4_COEX:BLE_SCAN_STOP_PASS",
                "RA4_COEX:BLE_END_CALLED",
                "RA4_COEX:WIFI_DISCONNECT_PASS",
                "RA4_COEX:ROUND_PASS=" + str(args.rounds),
                "RA4_COEX:RESULT=PASS",
            ),
            args.gate_timeout_ms / 1000.0,
            send_echo=True,
            expected_echoes=args.rounds,
        )
        normalized_capture = capture.replace("\r\n", "\n")
        if not args.stop_scan_before_tls:
            after_tls = re.findall(r"RA4_COEX:BLE_REPORTS_AFTER_TLS=(\d+)", capture)
            if len(after_tls) != args.rounds or any(int(n) == 0 for n in after_tls):
                raise RuntimeError("BLE reports did not resume after every TLS exchange")
        for round_number in range(1, args.rounds + 1):
            for marker in (
                f"RA4_COEX:ROUND_START={round_number}\n",
                f"RA4_COEX:TLS_ADDRESS_API_PASS={round_number}\n",
                f"RA4_COEX:ROUND_ELAPSED_MS={round_number},",
                f"RA4_COEX:ROUND_PASS={round_number}\n",
            ):
                if marker not in normalized_capture:
                    raise RuntimeError(f"coexistence cycle missing {marker.strip()}")
        print_markers("RA4_COEX", capture)
        peer_output += tls_gate.wait_for_peer_events(
            peer_event_log,
            (
                "TLS_DNS_PEER:TCP_ACCEPTED",
                "TLS_DNS_PEER:SNI_PASS=1",
                "TLS_DNS_PEER:EXCHANGE_PASS=" + str(args.rounds),
            ),
            10.0,
        )
        print("RA4_WIFI_BLE_TLS_USB_COEX_GATE_PASS")
    except MarkerCaptureError as error:
        print_markers("RA4_COEX_DIAGNOSTICS", error.captured)
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
            tls_gate.stop_windows_peer(peer, work_dir)
        tls_gate.remove_peer_firewall(firewall_rule)
        peer_output = tls_gate.read_peer_event_log(peer_event_log) or peer_output
        tls_gate.print_peer_markers("\n".join(
            line for line in peer_output.splitlines()
            if not line.startswith("TLS_DNS_PEER:SNI=")))
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
        if ra4_modified:
            artifacts = WORK_ROOT / ("artifacts-" + work_dir.name)
            artifacts.mkdir(exist_ok=True)
            for suffix in ("elf", "map", "bin"):
                output = work_dir / "build" / ("ra4-wifi-ble-tls-usb-coex.ino." + suffix)
                if output.is_file():
                    shutil.copy2(output, artifacts / output.name)
            print("RA4_COEX:BUILD_ARTIFACTS=" + str(artifacts))
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
