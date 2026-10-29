#!/usr/bin/env python3
"""Validate TLS close_notify, bare FIN, half-close, and slot reuse on BL616CL."""

from __future__ import annotations

import argparse
import ipaddress
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
    / "ra4-wifis3-tls-graceful-close-gate"
    / "ra4-wifis3-tls-graceful-close-gate.ino.in"
)
WORK_ROOT = ROOT / "build" / "ra4-wifis3-tls-graceful-close-gate"
OVERLAY_PREPARER = ROOT / "tools" / "prepare_wifis3_result_error_overlay.py"
SAMBA_RUNNER = ROOT / "tools" / "run_ra4_bossac_final_carrier.py"
TLS_PORT = 18452


WINDOWS_PEER = r"""
import argparse
import socket
import ssl

parser = argparse.ArgumentParser()
parser.add_argument("--bind-ip", required=True)
parser.add_argument("--port", required=True, type=int)
parser.add_argument("--cert", required=True)
parser.add_argument("--key", required=True)
parser.add_argument("--event-log", required=True)
parser.add_argument("--close-mode", choices=("graceful", "fin", "half-close", "mixed"), required=True)
parser.add_argument("--rounds", type=int, default=1)
parser.add_argument("--client-close-mode", choices=("notify", "baseline"), default="notify")
args = parser.parse_args()
if not 1 <= args.rounds <= 100:
    parser.error("rounds must be in 1..100")


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

def run_cycle(listener, close_mode, expected):
    first, _ = listener.accept()
    first.settimeout(20)
    with context.wrap_socket(first, server_side=True) as first_tls:
        emit("TLS_GRACEFUL_CLOSE_PEER:FIRST_HANDSHAKE_PASS")
        payload = recv_exact(first_tls, len(expected))
        if payload != expected:
            emit("TLS_GRACEFUL_CLOSE_PEER:FIRST_PAYLOAD_FAIL")
            raise SystemExit(2)
        first_tls.sendall(payload)
        emit("TLS_GRACEFUL_CLOSE_PEER:FIRST_ECHO_PASS")
        if close_mode == "graceful":
            try:
                first_tls.unwrap()
            except (ssl.SSLError, OSError):
                # The client may stop after observing the peer's close_notify.
                pass
            emit("TLS_GRACEFUL_CLOSE_PEER:CLOSE_NOTIFY_SENT")
        else:
            first_tls.sendall(bytes(byte ^ 0xFF for byte in expected))
            # Detach before TCP shutdown so SSLSocket cannot emit close_notify.
            with socket.socket(fileno=first_tls.detach()) as first_raw:
                first_raw.settimeout(20)
                if close_mode == "half-close":
                    first_raw.shutdown(socket.SHUT_WR)
                    emit("TLS_GRACEFUL_CLOSE_PEER:HALF_CLOSE_SENT")
                    after_fin = bytearray()
                    while True:
                        part = first_raw.recv(256)
                        if not part:
                            break
                        after_fin.extend(part)
                        if len(after_fin) > 2048:
                            raise RuntimeError("client sent excessive data after FIN")
                    # Some TLS clients send a fatal truncation alert before TCP
                    # close. Application write failure is checked on RA4M1.
                    emit("TLS_GRACEFUL_CLOSE_PEER:CLIENT_AFTER_FIN_BYTES=" + str(len(after_fin)))
                    emit("TLS_GRACEFUL_CLOSE_PEER:CLIENT_FIN_RECEIVED")
                else:
                    first_raw.close()
                    emit("TLS_GRACEFUL_CLOSE_PEER:BARE_FIN_SENT")

    second, _ = listener.accept()
    second.settimeout(20)
    second_tls = context.wrap_socket(
        second, server_side=True, suppress_ragged_eofs=False
    )
    emit("TLS_GRACEFUL_CLOSE_PEER:SECOND_HANDSHAKE_PASS")
    payload = recv_exact(second_tls, len(expected))
    if payload != expected:
        emit("TLS_GRACEFUL_CLOSE_PEER:RECOVERY_PAYLOAD_FAIL")
        raise SystemExit(2)
    second_tls.sendall(payload)
    emit("TLS_GRACEFUL_CLOSE_PEER:RECOVERY_ECHO_PASS")
    if args.client_close_mode == "baseline":
        try:
            if second_tls.recv(1) != b"":
                raise RuntimeError("unexpected application data after recovery echo")
        except ssl.SSLEOFError:
            emit("TLS_GRACEFUL_CLOSE_PEER:CLIENT_TCP_EOF_RECEIVED")
        else:
            raise RuntimeError("baseline stop unexpectedly sent TLS close_notify")
        finally:
            second_tls.close()
        return
    try:
        # Wait for the client to initiate TLS shutdown. Calling unwrap first
        # would send the server alert before RA4M1 reaches client.stop().
        if second_tls.recv(1) != b"":
            raise RuntimeError("unexpected application data after recovery echo")
        second_raw = second_tls.unwrap()
    except (ssl.SSLError, OSError, RuntimeError) as error:
        emit("TLS_GRACEFUL_CLOSE_PEER:CLIENT_CLOSE_NOTIFY_FAIL")
        emit("TLS_GRACEFUL_CLOSE_PEER:CLOSE_ERROR=" + str(error))
        raise SystemExit(3)
    second_raw.close()
    emit("TLS_GRACEFUL_CLOSE_PEER:CLIENT_CLOSE_NOTIFY_RECEIVED")

with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind((args.bind_ip, args.port))
    listener.listen(2)
    listener.settimeout(120)
    emit("TLS_GRACEFUL_CLOSE_PEER:READY")
    for cycle in range(1, args.rounds + 1):
        mode = ("graceful", "fin", "half-close")[(cycle - 1) % 3] if args.close_mode == "mixed" else args.close_mode
        expected = bytes((0x70 + index + cycle - 1) & 0xFF for index in range(32))
        emit("TLS_GRACEFUL_CLOSE_PEER:CYCLE_START=" + str(cycle) + "," + mode)
        run_cycle(listener, mode, expected)
        emit("TLS_GRACEFUL_CLOSE_PEER:CYCLE_PASS=" + str(cycle))

emit("TLS_GRACEFUL_CLOSE_PEER:RESULT=PASS")
"""


def run_command(command: list[str]) -> None:
    print("COMMAND=" + " ".join(command))
    if subprocess.run(command, check=False).returncode != 0:
        raise RuntimeError("subprocess failed")


def compile_sketch(
    work_dir: Path, trusted_ca: Path, arduino_cli: str,
    close_mode: str = "graceful", rounds: int = 1,
) -> Path:
    sketch_dir = work_dir / "ra4-wifis3-tls-graceful-close-gate"
    sketch_dir.mkdir(parents=True, exist_ok=True)
    (sketch_dir / "ra4-wifis3-tls-graceful-close-gate.ino").write_text(
        TEMPLATE.read_text(encoding="ascii").replace(
            "@TLS_TRUSTED_CA_LITERAL@",
            pem_c_literal(trusted_ca.read_text(encoding="ascii")),
        ).replace(
            "@TLS_EXPECT_ABRUPT_CLOSE@", "false" if close_mode == "graceful" else "true"
        ).replace(
            "@TLS_CLOSE_ROUNDS@", str(rounds)
        ).replace(
            "@TLS_MIXED_CLOSE@", "true" if close_mode == "mixed" else "false"
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
        arduino_cli,
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
    image = build_dir / "ra4-wifis3-tls-graceful-close-gate.ino.bin"
    if not image.is_file():
        raise RuntimeError("compiled graceful-close image is missing")
    return image


def wait_ready(link: serial.Serial, timeout_seconds: float) -> None:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    while time.monotonic() < deadline:
        received.extend(link.read(256))
        if b"RA4_TLS_GRACEFUL_CLOSE:READY" in received:
            return
        if len(received) > 4096:
            del received[:-1024]
    raise RuntimeError("RA4M1 graceful-close Sketch did not start")


def validate_result(
    text: str, close_mode: str, rounds: int, max_idle_heap_loss_bytes: int = 2048,
) -> list[str]:
    text = text.replace("\r\n", "\n")
    cycles = re.findall(r"^RA4_TLS_GRACEFUL_CLOSE:CYCLE_PASS=(\d+)\s*$", text, re.M)
    if cycles != [str(cycle) for cycle in range(1, rounds + 1)]:
        raise RuntimeError("TLS close gate did not complete every requested cycle")
    sections = re.split(r"^RA4_TLS_GRACEFUL_CLOSE:CYCLE_START=\d+\s*$", text, flags=re.M)[1:]
    if len(sections) != rounds:
        raise RuntimeError("missing TLS close cycle boundaries")
    totals = re.findall(r"^RA4_TLS_GRACEFUL_CLOSE:HANDSHAKES_PASS=(\d+)\s*$", text, re.M)
    if totals != [str(rounds * 2)]:
        raise RuntimeError("incorrect TLS handshake total")
    diagnostics = []
    for index, section in enumerate(sections):
        mode = ("graceful", "fin", "half-close")[index % 3] if close_mode == "mixed" else close_mode
        required = (
            "FIRST_HANDSHAKE_PASS", "FIRST_ECHO_PASS",
            "CONNECTED_AFTER_CLOSE=0", "AVAILABLE_AFTER_CLOSE=0", "READ_AFTER_CLOSE=-1",
            "CLOSE_NOTIFY_PASS" if mode == "graceful" else "BARE_FIN_PASS",
            "WRITE_AFTER_CLOSE=0", "RECOVERY_HANDSHAKE_PASS", "RECOVERY_ECHO_PASS",
        )
        for marker in required:
            if "RA4_TLS_GRACEFUL_CLOSE:" + marker not in section:
                raise RuntimeError(f"cycle {index + 1} missing close marker: {marker}")
        if mode != "graceful" and "RA4_TLS_GRACEFUL_CLOSE:TAIL_DATA_PASS" not in section:
            raise RuntimeError(f"cycle {index + 1} missing tail data")
        elapsed = re.findall(r"^RA4_TLS_GRACEFUL_CLOSE:CYCLE_ELAPSED_MS=(\d+)\s*$", section, re.M)
        if len(elapsed) != 1 or int(elapsed[0]) <= 0:
            raise RuntimeError(f"cycle {index + 1} missing elapsed time")
        if rounds > 1:
            for stage in ("FIRST", "RECOVERY", "IDLE"):
                match = re.search(r"^RA4_TLS_GRACEFUL_CLOSE:" + stage + r"_DIAG=([^\r\n]+)$", section, re.M)
                if match is None:
                    raise RuntimeError(f"cycle {index + 1} missing {stage} resource diagnostic")
                try:
                    values = [int(value) for value in match[1].split(",")]
                except ValueError as error:
                    raise RuntimeError("invalid TLS heap diagnostic") from error
                # Layout-1 TLS flow telemetry is an optional append-only
                # suffix; retain the original resource/heap field positions.
                valid_layout = len(values) in (15, 44) or (
                    len(values) == 62 and values[44] == 1)
                if not valid_layout or values[:2] != ([0, 0] if stage == "IDLE" else [6, 0]):
                    raise RuntimeError(f"cycle {index + 1} unexpected {stage} resource diagnostic")
                if stage == "IDLE":
                    diagnostics.append(values[13:15])
    summaries = []
    if diagnostics:
        for column, name in enumerate(("OCRAM", "WRAM")):
            samples = [values[column] for values in diagnostics]
            summaries.append(f"TLS_CLOSE_IDLE_{name}_RANGE_BYTES={min(samples)},{max(samples)}")
            if rounds >= 6:
                # Skip two warm-up cycles and compare low-water samples in
                # early/late windows; diagnostics have 1 KiB quantization.
                baseline = min(samples[2:5])
                final = min(samples[-3:])
                loss = baseline - final
                summaries.append(f"TLS_CLOSE_IDLE_{name}_BASELINE_FINAL_LOSS_BYTES={baseline},{final},{loss}")
                if loss > max_idle_heap_loss_bytes:
                    raise RuntimeError(f"idle {name} lost {loss} bytes across TLS cycles")
    return summaries


def capture_result(
    link: serial.Serial, timeout_seconds: float, close_mode: str = "graceful",
    rounds: int = 1, max_idle_heap_loss_bytes: int = 2048,
) -> str:
    deadline = time.monotonic() + timeout_seconds
    received = bytearray()
    pending = bytearray()
    while time.monotonic() < deadline:
        chunk = link.read(512)
        received.extend(chunk)
        pending.extend(chunk)
        while b"\n" in pending:
            line, _, pending = pending.partition(b"\n")
            print_markers(line.decode("ascii", errors="replace"))
        if len(received) > 256 * 1024:
            raise RuntimeError("TLS close capture exceeded its byte limit")
        text = received.decode("ascii", errors="replace").replace("\x00", "")
        if "RA4_TLS_GRACEFUL_CLOSE:RESULT=FAIL:" in text:
            print_markers(text)
            raise RuntimeError("TLS graceful-close gate failed")
        if "RA4_TLS_GRACEFUL_CLOSE:RESULT=PASS" in text:
            for summary in validate_result(text, close_mode, rounds, max_idle_heap_loss_bytes):
                print(summary, flush=True)
            return text
    raise RuntimeError("TLS graceful-close gate timed out")


def print_markers(text: str) -> None:
    for line in text.replace("\x00", "").splitlines():
        if line.strip().startswith("RA4_TLS_GRACEFUL_CLOSE:"):
            print(line.strip(), flush=True)


def launch_peer(
    work_dir: Path,
    host: ipaddress.IPv4Address,
    certificate: Path,
    key: Path,
    event_log: Path,
    close_mode: str = "graceful",
    rounds: int = 1,
    client_close_mode: str = "baseline",
) -> subprocess.Popen[str]:
    script = work_dir / "windows_tls_peer_graceful_close.py"
    script.write_text(textwrap.dedent(WINDOWS_PEER), encoding="ascii")
    return subprocess.Popen(
        [
            "powershell.exe", "-NoProfile", "-Command",
            (
                f'& py.exe -3 "{wsl_unc_path(script)}" '
                f"--bind-ip {host} --port {TLS_PORT} "
                f'--cert "{wsl_unc_path(certificate)}" '
                f'--key "{wsl_unc_path(key)}" '
                f'--event-log "{wsl_unc_path(event_log)}"'
                f" --close-mode {close_mode}"
                f" --rounds {rounds}"
                f" --client-close-mode {client_close_mode}"
            ),
        ],
        stdout=subprocess.DEVNULL,
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


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--skip-peer-firewall", action="store_true")
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--client-close-mode", choices=("baseline", "notify"), default="baseline",
                        help="baseline TCP close or optional strict TLS close_notify check")
    parser.add_argument("--close-mode", choices=("graceful", "fin", "half-close", "mixed"), default="graceful")
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--max-idle-heap-loss-bytes", type=int, default=2048)
    args = parser.parse_args()
    if not 1 <= args.rounds <= 100 or args.max_idle_heap_loss_bytes < 0:
        parser.error("rounds must be in 1..100 and heap-loss allowance nonnegative")
    WORK_ROOT.mkdir(parents=True, exist_ok=True)
    work_dir = Path(tempfile.mkdtemp(prefix="run-", dir=WORK_ROOT))
    peer = None
    bridge = None
    firewall = None
    ra4_modified = False
    error: Exception | None = None
    events = work_dir / "peer.events"
    try:
        material_dir = work_dir / "material"
        material_dir.mkdir()
        host = ipaddress.IPv4Address(
            "192.0.2.1" if args.source_only else windows_wlan_address()
        )
        ca, certificate, key = create_material(material_dir, "ec", host)
        image = compile_sketch(work_dir, ca, args.arduino_cli, args.close_mode, args.rounds)
        if args.source_only:
            print("RA4_WIFIS3_TLS_GRACEFUL_CLOSE_SOURCE_COMPILE_PASS")
            return 0

        args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
        final_scan.require_final_carrier_bridge(args.bridge_port)
        ssid, password = load_private_credentials(args.credential_env)
        if password is None:
            raise RuntimeError("private WPA password required")
        if not args.skip_peer_firewall:
            firewall = configure_peer_firewall(str(host), TLS_PORT)
        peer = launch_peer(work_dir, host, certificate, key, events, args.close_mode, args.rounds, args.client_close_mode)
        wait_for_peer_events(
            events,
            ("TLS_GRACEFUL_CLOSE_PEER:READY",),
            15.0,
        )
        ra4_modified = True
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
        output = capture_result(
            bridge, 60.0 + args.rounds * 60.0, args.close_mode,
            args.rounds, args.max_idle_heap_loss_bytes,
        )
        peer_output = wait_for_peer_events(
            events,
            (
                "TLS_GRACEFUL_CLOSE_PEER:FIRST_HANDSHAKE_PASS",
                "TLS_GRACEFUL_CLOSE_PEER:FIRST_ECHO_PASS",
                {
                    "graceful": "TLS_GRACEFUL_CLOSE_PEER:CLOSE_NOTIFY_SENT",
                    "fin": "TLS_GRACEFUL_CLOSE_PEER:BARE_FIN_SENT",
                    "half-close": "TLS_GRACEFUL_CLOSE_PEER:CLIENT_FIN_RECEIVED",
                    "mixed": "TLS_GRACEFUL_CLOSE_PEER:CLIENT_FIN_RECEIVED" if args.rounds >= 3
                    else "TLS_GRACEFUL_CLOSE_PEER:CLOSE_NOTIFY_SENT",
                }[args.close_mode],
                "TLS_GRACEFUL_CLOSE_PEER:SECOND_HANDSHAKE_PASS",
                "TLS_GRACEFUL_CLOSE_PEER:RECOVERY_ECHO_PASS",
                ("TLS_GRACEFUL_CLOSE_PEER:CLIENT_TCP_EOF_RECEIVED"
                 if args.client_close_mode == "baseline"
                 else "TLS_GRACEFUL_CLOSE_PEER:CLIENT_CLOSE_NOTIFY_RECEIVED"),
                "TLS_GRACEFUL_CLOSE_PEER:RESULT=PASS",
            ),
            15.0,
        )
        peer_cycles = re.findall(r"^TLS_GRACEFUL_CLOSE_PEER:CYCLE_PASS=(\d+)\s*$", peer_output, re.M)
        if peer_cycles != [str(cycle) for cycle in range(1, args.rounds + 1)]:
            raise RuntimeError("TLS peer did not complete every requested cycle")
        for line in peer_output.splitlines():
            if line.startswith("TLS_GRACEFUL_CLOSE_PEER:"):
                print(line)
        print("RA4_WIFIS3_TLS_GRACEFUL_CLOSE_GATE_PASS")
        print(f"RA4_WIFIS3_TLS_CLOSE_MODE_PASS={args.close_mode}")
        print(f"RA4_WIFIS3_TLS_CLOSE_CYCLES_PASS={args.rounds}")
    except (
        OSError,
        RuntimeError,
        serial.SerialException,
        subprocess.SubprocessError,
    ) as caught:
        error = caught
    finally:
        if bridge is not None and bridge.is_open:
            bridge.close()
        if peer is not None:
            stop_windows_peer(peer, work_dir)
        remove_peer_firewall(firewall)
        peer_output = read_peer_event_log(events)
        if error is not None:
            for line in peer_output.splitlines():
                if line.startswith("TLS_GRACEFUL_CLOSE_PEER:"):
                    print(line)
        if ra4_modified:
            try:
                restore_scan(args)
            except (
                OSError,
                RuntimeError,
                serial.SerialException,
                subprocess.SubprocessError,
            ) as restore_error:
                print(
                    "RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_FAILED",
                    file=sys.stderr,
                )
                if error is None:
                    error = restore_error
        shutil.rmtree(work_dir, ignore_errors=True)
    if error is not None:
        raise error
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
