#!/usr/bin/env python3
"""Exercise four WiFiS3 UDP sockets with a real Windows WLAN echo peer."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
import tempfile
import textwrap
import time
from pathlib import Path

import serial

sys.path.insert(0, str(Path(__file__).resolve().parent))
from run_ra4_wifis3_tcp_resource_gate import (
    encode_start_value,
    print_markers,
    read_peer_event_log,
    run_command,
    stop_windows_peer,
    windows_wlan_address,
    wsl_unc_path,
)
from run_bl616cl_apsta_gate import DEFAULT_CREDENTIAL_ENV, load_private_credentials
from run_ra4_bossac_final_carrier import require_final_carrier_bridge
import run_ra4_wifis3_scan_final_carrier as final_scan
from uno_r4_usb_identity import resolve_paired_bl616cl_port


ROOT = Path(__file__).resolve().parents[1]
SKETCH = ROOT / "examples" / "ra4-wifis3-udp-resource-gate"
BUILD = ROOT / "build" / "ra4-wifis3-udp-resource-gate"
IMAGE = BUILD / "ra4-wifis3-udp-resource-gate.ino.bin"
WORK = ROOT / "build" / "ra4-wifis3-udp-resource-peer"
PORT = 18448
EXCHANGES = 13

PEER = r"""
import argparse
import socket
import time
import os
import ctypes

# System-wide IPv4 counters, read-only. No packet payload or interface names.
def ipv4_reassembly_counters():
    if os.name != "nt":
        return None
    counters = (ctypes.c_uint32 * 23)()
    if ctypes.windll.iphlpapi.GetIpStatistics(ctypes.byref(counters)) != 0:
        return None
    return tuple(counters[index] for index in (14, 15, 16, 7))

parser = argparse.ArgumentParser()
parser.add_argument("--bind-ip", required=True)
parser.add_argument("--port", required=True, type=int)
parser.add_argument("--event-log", required=True)
parser.add_argument("--cycles", type=int, required=True)
parser.add_argument("--large-datagrams", type=int, default=1)
parser.add_argument("--varied-payload", action="store_true")
args = parser.parse_args()
if not 1 <= args.large_datagrams <= 20:
    raise ValueError("invalid large datagram count")
if not 1 <= args.cycles <= 3:
    raise ValueError("invalid cycle count")
expected = 13 * args.cycles
overlap_probe = b"bl616cl-udp-overlap-probe"
failure_probe = b"bl616cl-udp-post-failure-probe"
valid = {
    f"bl616cl-udp-resource-r{cycle * 4 + round}-s{slot}".encode("ascii")
    for cycle in range(args.cycles) for round in range(3) for slot in range(4)
}
valid.update(
    f"bl616cl-udp-resource-r{cycle * 4 + 3}-s1".encode("ascii")
    for cycle in range(args.cycles)
)
seen = set()
max_datagrams_received = set()
with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as peer:
    peer.bind((args.bind_ip, args.port))
    peer.settimeout(0.5)
    print("UDP_RESOURCE_PEER:READY", flush=True)
    with open(args.event_log, "a", encoding="ascii") as events:
        events.write("UDP_RESOURCE_PEER:READY\n")
    initial_ip = ipv4_reassembly_counters()
    last_ip = None
    deadline = time.monotonic() + (240 if args.cycles > 1 else 90) + 12 * (args.large_datagrams - 1)
    while time.monotonic() < deadline and (
        len(seen) < expected + 1 or len(max_datagrams_received) < args.large_datagrams
    ):
        current_ip = ipv4_reassembly_counters()
        if initial_ip is not None and current_ip is not None:
            delta = tuple((value - base) & 0xffffffff
                          for value, base in zip(current_ip, initial_ip))
            if delta != last_ip:
                with open(args.event_log, "a", encoding="ascii") as events:
                    events.write("UDP_RESOURCE_PEER:IPV4_REASSEMBLY_DELTA="
                                 + ",".join(map(str, delta)) + "\n")
                last_ip = delta
        try:
            message, source = peer.recvfrom(16385)
        except socket.timeout:
            continue
        sequence = message[0] if len(message) == 16384 else -1
        expected_payload = (bytes((sequence + offset * 19) & 255 for offset in range(16384))
                            if args.varied_payload and sequence >= 0
                            else bytes([max(0, sequence)]) * 16384)
        if (0 <= sequence < args.large_datagrams and message == expected_payload):
            max_datagrams_received.add(sequence)
            with open(args.event_log, "a", encoding="ascii") as events:
                events.write("UDP_RESOURCE_PEER:OVERLAP_16K_RECEIVED\n")
                events.write("UDP_RESOURCE_PEER:LARGE_DATAGRAM_RECEIVED=" + str(sequence + 1) + "\n")
            receipt = b"bl616cl-udp-16k-received"
            if sequence:
                receipt += b"-" + str(sequence).encode("ascii")
            peer.sendto(receipt, source)
            continue
        if len(message) >= 16384:
            mismatch = next((index for index, (actual, wanted) in
                             enumerate(zip(message, expected_payload)) if actual != wanted), -1)
            with open(args.event_log, "a", encoding="ascii") as events:
                events.write("UDP_RESOURCE_PEER:LARGE_PAYLOAD_REJECT="
                             + str(len(message)) + "," + str(mismatch) + "\n")
            continue
        if message == overlap_probe:
            peer.sendto(message, source)
            seen.add(message)
            with open(args.event_log, "a", encoding="ascii") as events:
                events.write("UDP_RESOURCE_PEER:OVERLAP_PROBE_ECHO\n")
            continue
        if message == failure_probe:
            peer.sendto(message, source)
            with open(args.event_log, "a", encoding="ascii") as events:
                events.write("UDP_RESOURCE_PEER:POST_FAILURE_SMALL_ECHO\n")
            continue
        if message not in valid or message in seen:
            continue
        peer.sendto(message, source)
        seen.add(message)
        with open(args.event_log, "a", encoding="ascii") as events:
            events.write("UDP_RESOURCE_PEER:ECHO_PASS=" + str(len(seen)) + "\n")
"""


def configure_firewall(ip: object, port: int) -> str:
    name = f"BL616CL-UDP-RESOURCE-{os.getpid()}"
    result = subprocess.run(
        [
            "powershell.exe", "-NoProfile", "-Command",
            '$ErrorActionPreference = "Stop"; '
            f'New-NetFirewallRule -DisplayName "{name}" -Direction Inbound '
            f'-Action Allow -Profile Any -Protocol UDP -LocalAddress {ip} '
            f'-LocalPort {port} | Out-Null; Write-Output "FIREWALL_READY"',
        ],
        capture_output=True, text=True, encoding="utf-8", errors="replace",
        check=False,
    )
    if result.returncode != 0 or "FIREWALL_READY" not in result.stdout:
        raise RuntimeError("cannot create temporary Windows UDP firewall rule")
    print("UDP_RESOURCE_PEER:FIREWALL_READY")
    return name


def remove_firewall(name: str | None) -> None:
    if name:
        subprocess.run(
            ["powershell.exe", "-NoProfile", "-Command",
             f'Remove-NetFirewallRule -DisplayName "{name}" -ErrorAction SilentlyContinue'],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False,
        )


def compile_sketch(cli: str, large_datagrams: int = 1, varied_payload: bool = False, fragment_diagnostics: bool = False, failure_connectivity_probe: bool = False) -> None:
    run_command([cli, "compile", "--clean", "--fqbn",
                 "arduino:renesas_uno:unor4wifi", "--build-property",
                 f"compiler.cpp.extra_flags=-DUDP_LARGE_DATAGRAMS={large_datagrams} -DUDP_VARIED_PAYLOAD={int(varied_payload)} -DUDP_FRAGMENT_DIAGNOSTICS={int(fragment_diagnostics)} -DUDP_FAILURE_CONNECTIVITY_PROBE={int(failure_connectivity_probe)}",
                 "--build-path", str(BUILD), str(SKETCH)])
    if not IMAGE.is_file():
        raise RuntimeError("compiled UDP resource image is missing")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bridge-port", default="auto")
    parser.add_argument("--arduino-cli", default="arduino-cli")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--udp-port", type=int, default=PORT)
    parser.add_argument("--skip-peer-firewall", action="store_true")
    parser.add_argument("--stress", action="store_true",
                        help="three bounded STA/socket reconnect cycles")
    parser.add_argument("--large-datagrams", type=int, default=1,
                        help="1..20 consecutive 16 KiB datagrams on the same STA/socket")
    parser.add_argument("--varied-payload", action="store_true",
                        help="diagnostic varied-byte pattern instead of uniform packet bytes")
    parser.add_argument("--fragment-diagnostics", action="store_true",
                        help="record passive fragment content counters for every delivered packet")
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--failure-connectivity-probe", action="store_true",
                        help="after a failed large receipt, try one small echo without changing the FAIL verdict")
    args = parser.parse_args()
    if not 1 <= args.large_datagrams <= 20:
        parser.error("large datagrams must be 1..20")
    if not (1 <= args.udp_port <= 65534):
        raise RuntimeError("invalid UDP port")
    if args.source_only:
        compile_sketch(args.arduino_cli, args.large_datagrams, args.varied_payload, args.fragment_diagnostics, args.failure_connectivity_probe)
        print("RA4_WIFIS3_UDP_RESOURCE_SOURCE_COMPILE_PASS")
        return 0

    args.bridge_port = resolve_paired_bl616cl_port(args.bridge_port)
    require_final_carrier_bridge(args.bridge_port)
    ssid, password = load_private_credentials(args.credential_env)
    if password is None:
        raise RuntimeError("private WPA password required")
    ip = windows_wlan_address()
    print(f"Bridge: {args.bridge_port} (verified BL616CL final carrier)")
    print("Target: temporary RA4M1 Sketch; BL616CL Flash unchanged.")
    WORK.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="udp-resource-", dir=WORK))
    events = directory / "peer.events"
    peer = None
    bridge = None
    firewall = None
    ra4_modified = False
    error = None
    try:
        compile_sketch(args.arduino_cli, args.large_datagrams, args.varied_payload, args.fragment_diagnostics, args.failure_connectivity_probe)
        if args.skip_peer_firewall:
            print("UDP_RESOURCE_PEER:FIREWALL_SKIPPED")
        else:
            firewall = configure_firewall(ip, args.udp_port)
        script = directory / "peer.py"
        script.write_text(textwrap.dedent(PEER), encoding="ascii")
        peer = subprocess.Popen(
            ["powershell.exe", "-NoProfile", "-Command",
             f'& py.exe -3 "{wsl_unc_path(script)}" --bind-ip {ip} '
             f'--port {args.udp_port} --event-log "{wsl_unc_path(events)}" '
             f'--cycles {3 if args.stress else 1} --large-datagrams {args.large_datagrams}'
             + (' --varied-payload' if args.varied_payload else '')],
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
            encoding="utf-8", errors="replace",
        )
        wait_peer_ready(peer, events, 15.0)
        ra4_modified = True
        run_command([
            sys.executable, str(ROOT / "tools" / "run_ra4_bossac_final_carrier.py"),
            "--bridge-port", args.bridge_port, "--image", str(IMAGE),
            "--runtime-settle-ms", "1000", "--attempts", "3",
        ])
        bridge = serial.Serial(args.bridge_port, baudrate=115200,
                               timeout=0.1, write_timeout=1)
        bridge.dtr = False
        bridge.rts = False
        bridge.reset_input_buffer()
        wait_ready(bridge, 15.0)
        bridge.reset_input_buffer()
        bridge.write(
            f"START {encode_start_value(ssid)} {encode_start_value(password)} "
            f"{ip} {args.udp_port}{' STRESS' if args.stress else ''}\n".encode("ascii")
        )
        bridge.flush()
        # The TCP helper detects TCP-specific FAIL markers; check UDP failures
        # explicitly by capturing through the final marker and inspecting all output.
        output = capture_udp(bridge, (240 if args.stress else 95) + 12 * (args.large_datagrams - 1),
                             3 if args.stress else 1, args.large_datagrams)
        print_markers("RA4_UDP_RESOURCE_RUNTIME", output, "RA4_UDP_RESOURCE:")
        peer_events = read_peer_event_log(events)
        expected = EXCHANGES * (3 if args.stress else 1)
        if f"UDP_RESOURCE_PEER:ECHO_PASS={expected}\n" not in peer_events:
            raise RuntimeError("Windows UDP peer did not confirm all unique echoes")
        if ("UDP_RESOURCE_PEER:OVERLAP_PROBE_ECHO\n" not in peer_events or
            "UDP_RESOURCE_PEER:OVERLAP_16K_RECEIVED\n" not in peer_events):
            raise RuntimeError("Windows UDP peer did not confirm overlapping 16 KiB datagram")
        for sequence in range(1, args.large_datagrams + 1):
            marker = f"UDP_RESOURCE_PEER:LARGE_DATAGRAM_RECEIVED={sequence}\n"
            if marker not in peer_events:
                raise RuntimeError(f"Windows UDP peer missing large datagram {sequence}")
            print(marker.strip())
        print(f"UDP_RESOURCE_PEER:ECHO_PASS={expected}")
        print("UDP_RESOURCE_PEER:OVERLAP_16K_RECEIVED")
        print("RA4_WIFIS3_UDP_STRESS_GATE_PASS" if args.stress else
              "RA4_WIFIS3_UDP_RESOURCE_GATE_PASS")
    except (OSError, RuntimeError, serial.SerialException,
            subprocess.SubprocessError) as caught:
        error = caught
    finally:
        if bridge is not None and bridge.is_open:
            bridge.close()
        if peer is not None:
            stop_windows_peer(peer, directory)
        remove_firewall(firewall)
        if error is not None:
            for line in read_peer_event_log(events).splitlines():
                if line.startswith("UDP_RESOURCE_PEER:"):
                    print(line)
        if ra4_modified:
            try:
                run_command([
                    sys.executable, str(final_scan.__file__),
                    "--bridge-port", args.bridge_port, "--arduino-cli", args.arduino_cli,
                ])
                print("RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_PASS")
            except (OSError, RuntimeError, subprocess.SubprocessError):
                print("RA4_WIFI_S3_SCAN_FINAL_CARRIER_RESTORE_FAILED", file=sys.stderr)
                if error is None:
                    error = RuntimeError("RA4M1 scan restore failed")
        shutil.rmtree(directory, ignore_errors=True)
    if error is not None:
        raise error
    return 0


def capture_udp(bridge: serial.Serial, timeout: float, cycles: int, large_datagrams: int = 1) -> str:
    deadline = time.monotonic() + timeout
    received = bytearray()
    while time.monotonic() < deadline:
        received.extend(bridge.read(512))
        if len(received) > 16384:
            raise RuntimeError("RA4M1 UDP output exceeded limit")
        text = received.decode("ascii", errors="replace").replace(
            "\x00", "").replace("\r", "")
        if "RA4_UDP_RESOURCE:RESULT=FAIL:" in text:
            print_markers("RA4_UDP_RESOURCE_DIAGNOSTICS", text, "RA4_UDP_RESOURCE:")
            raise RuntimeError("RA4M1 UDP resource gate failed")
        if "RA4_UDP_RESOURCE:RESULT=PASS" in text:
            required = [
                "RA4_UDP_RESOURCE:FOUR_OPEN_PASS",
                "RA4_UDP_RESOURCE:REJECT_PASS",
                *(f"RA4_UDP_RESOURCE:ROUND_PASS={i}" for i in range(1, 4)),
                "RA4_UDP_RESOURCE:REBIND_PASS",
                "RA4_UDP_RESOURCE:OVERLAP_RECEIVE_PENDING_PASS",
                "RA4_UDP_RESOURCE:OVERLAP_16K_SEND_PASS",
                "RA4_UDP_RESOURCE:OVERLAP_RECEIVE_READ_PASS",
                "RA4_UDP_RESOURCE:OVERLAP_PEER_ACK_PASS",
            ]
            if not all(marker in text for marker in required):
                print_markers("RA4_UDP_RESOURCE_DIAGNOSTICS", text, "RA4_UDP_RESOURCE:")
                raise RuntimeError("RA4M1 UDP gate missing required markers")
            if large_datagrams > 1:
                for sequence in range(1, large_datagrams + 1):
                    if f"RA4_UDP_RESOURCE:LARGE_DATAGRAM_PASS={sequence}\n" not in text:
                        raise RuntimeError(f"RA4M1 missing large datagram {sequence}")
            cycle_markers = [
                f"RA4_UDP_RESOURCE:CYCLE_PASS={i}\n"
                for i in range(1, cycles + 1)
            ]
            if (text.count("RA4_UDP_RESOURCE:FOUR_OPEN_PASS") != cycles or
                text.count("RA4_UDP_RESOURCE:REBIND_PASS") != cycles or
                (cycles > 1 and any(marker not in text for marker in cycle_markers)) or
                (cycles > 1 and "RA4_UDP_RESOURCE:ELAPSED_MS=" not in text)):
                print_markers("RA4_UDP_RESOURCE_DIAGNOSTICS", text, "RA4_UDP_RESOURCE:")
                raise RuntimeError("RA4M1 UDP gate missing reconnect cycles")
            return text
    print_markers("RA4_UDP_RESOURCE_DIAGNOSTICS", received.decode(
        "ascii", errors="replace"), "RA4_UDP_RESOURCE:")
    raise RuntimeError("RA4M1 UDP resource gate timed out")


def wait_ready(bridge: serial.Serial, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    received = bytearray()
    while time.monotonic() < deadline:
        received.extend(bridge.read(256))
        if len(received) > 4096:
            del received[:-1024]
        if b"RA4_UDP_RESOURCE:READY" in received:
            return
    raise RuntimeError("RA4M1 UDP resource Sketch did not start")


def wait_peer_ready(peer: subprocess.Popen[str], events: Path, timeout: float) -> None:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if "UDP_RESOURCE_PEER:READY" in read_peer_event_log(events):
            return
        if peer.poll() is not None:
            raise RuntimeError("Windows UDP peer exited before binding")
        time.sleep(0.05)
    raise RuntimeError("Windows UDP peer did not become ready")


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, serial.SerialException,
            subprocess.SubprocessError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        raise SystemExit(1)
