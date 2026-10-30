#!/usr/bin/env python3
"""Validate GATT, Notify or Indicate with Windows and restore the dedicated board."""

import subprocess
import argparse
import sys
import time
import tempfile
import ipaddress
import shutil
import re
from pathlib import Path

import serial

from run_bl616cl_generic_at_final_carrier import CLI, ROOT, restore_usb, run
from run_ra4_wifis3_scan_final_carrier import require_final_carrier_bridge
from uno_r4_usb_identity import resolve_paired_bl616cl_port
from uno_r4_usb_identity import resolve_baseline_esp32_port
import run_esp32_sta_baseline_gate as esp32_baseline
from run_bl616cl_apsta_gate import DEFAULT_CREDENTIAL_ENV, load_private_credentials
import run_ra4_wifis3_tls_gate as tls_gate
from run_ra4_wifi_ble_tls_usb_coex_gate import launch_windows_coex_peer, encode_start_value


def validate_heap_boundaries(lines, cycles):
    expected = [(0, stage) for stage in ("BEFORE_BLE", "AFTER_BLE", "ADVERTISING")]
    expected += [(cycle, stage) for cycle in range(1, cycles + 1)
                 for stage in ("BEFORE_TLS", "TLS_CONNECTED", "TLS_STOPPED")]
    rows = []
    for line in lines:
        match = re.fullmatch(r"BLE_TLS_COEX:HEAP_BOUNDARY=(\d+),([A-Z_]+),([\d,-]+)", line)
        if match is None:
            raise RuntimeError("malformed BLE/TLS heap boundary")
        values = [int(value) for value in match[3].split(",")]
        if len(values) != 19 or any(value < 0 for i, value in enumerate(values) if i != 1):
            raise RuntimeError("invalid BLE/TLS numeric heap prefix")
        rows.append((int(match[1]), match[2]))
    if rows != expected:
        raise RuntimeError("missing or unordered BLE/TLS heap boundaries")


def validate_hold_evidence(evidence, cycles, seconds):
    rows = re.findall(r"(?m)^WINDOWS_BLE_COEX:HOLD_PASS=(\d+),(\d+)\s*$", evidence)
    expected = list(range(1, cycles + 1)) if seconds else []
    if ([int(row[0]) for row in rows] != expected
            or any(int(row[1]) < seconds * 1000 for row in rows)):
        raise RuntimeError("Missing, unordered or short connected hold evidence")
    total = sum(int(row[1]) for row in rows)
    if seconds:
        print(f"BLE_WIFI_COEX:HOLD_TOTAL_MS={total}", flush=True)
    return total


def public_tls_events(path):
    return "\n".join(
        line for line in path.read_text(encoding="ascii").splitlines()
        if line.startswith("TLS_DNS_PEER:") and not line.startswith("TLS_DNS_PEER:SNI=")) + "\n"


def open_gate_serial(port, baseline):
    if baseline:
        link = esp32_baseline.open_serial(port, 0.2)
        link.write_timeout = 2
        return link
    link = serial.Serial(port, 115200, timeout=0.2, write_timeout=2)
    link.dtr = False
    link.rts = False
    return link


def discovery_failure_kind(evidence):
    if "Could not get GATT services: Unreachable" in evidence:
        return "UNREACHABLE"
    # A timeout in payload/signal handling is not a discovery failure.
    if ("WINDOWS_BLE_WINRT:DISCOVERY_CANCELLED=" in evidence
            and re.search(r"(?m)^TimeoutError\s*$", evidence)):
        return "DISCOVERY_TIMEOUT"
    return None


def require_controller_cs2(values, expected):
    if expected is not None and (not values or any(value != expected for value in values)):
        raise RuntimeError("Controller CSA2 feature does not match this control experiment")


def acknowledge_logical_disconnect(signals, completed):
    if (signals / f"disconnect-request-{completed + 1}.signal").is_file():
        completed += 1
        (signals / f"disconnect-{completed}.signal").write_text("PASS\n", encoding="ascii")
    return completed


def validate_backend_compatibility(evidence, sessions, connected, disconnected, logical_disconnected):
    rows = [int(value) for value in re.findall(r"(?m)^WINDOWS_BLE_COEX:SESSION_PASS=(\d+)\s*$", evidence)]
    retries = [int(value) for value in re.findall(r"(?m)^WINDOWS_BLE_WINRT:DISCOVERY_RETRY=(\d+)\s*$", evidence)]
    if (rows != list(range(1, sessions + 1)) or logical_disconnected != sessions + 1
            or connected < sessions + 1 or disconnected != connected
            or retries != list(range(1, len(retries) + 1))):
        raise RuntimeError("Backend compatibility session/link evidence incomplete")
    print(f"BLE_WINDOWS_BACKEND_COMPATIBILITY:SESSIONS={sessions}:LINKS={connected}:RETRIES={len(retries)}",
          flush=True)


def stop_discovery(port, baseline):
    try:
        with open_gate_serial(port, baseline) as cleanup:
            cleanup.write(b"Q")
            cleanup.flush()
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                if cleanup.readline().strip() == b"RA4_BLE_DISCOVERY:STOPPED":
                    print("RA4_BLE_DISCOVERY:STOPPED", flush=True)
                    return True
    except Exception as error:
        print("RA4_BLE_DISCOVERY:STOP_FAILED=" + type(error).__name__, flush=True)
    print("RA4_BLE_DISCOVERY:STOP_UNCONFIRMED", flush=True)
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("long-gatt", "notify", "indicate", "long-notify"), default="long-gatt")
    parser.add_argument("--peer-log", type=Path)
    parser.add_argument("--windows-ble-venv-name", default="uno-r4-ble-host-venv",
                        help="Name of an existing BLE Python environment under Windows TEMP")
    parser.add_argument("--backend-retry-compatibility", action="store_true",
                        help="Separate gate: allow backend retries; require logical sessions and all physical disconnects")
    parser.add_argument("--connected-coex", action="store_true")
    parser.add_argument("--no-wifi-scan", action="store_true",
                        help="Control: replace each Wi-Fi scan with a 3100 ms idle delay")
    parser.add_argument("--discovery-only", action="store_true",
                        help="Repeat full discovery and read without Wi-Fi scans or writes")
    parser.add_argument("--esp32-baseline", action="store_true",
                        help="Compare discovery or scan coexistence on the authorized ESP32-S3 board")
    parser.add_argument("--tls-coex", action="store_true")
    parser.add_argument("--sta-after-connect", action="store_true")
    parser.add_argument("--hci-diagnostics", action="store_true")
    parser.add_argument("--hci-credit-only", action="store_true",
                        help="Log connection/credit stalls without logging each ACL packet")
    parser.add_argument("--targeted-service-discovery", action="store_true",
                        help="Control experiment: request only the test service UUID on Windows")
    parser.add_argument("--hci-snapshot", action="store_true",
                        help="Count HCI traffic silently and request a snapshot on failure")
    parser.add_argument("--expected-local-cs2", type=int, choices=(0, 1),
                        help="Require the observed local LE CSA2 feature for a controller experiment")
    parser.add_argument("--expected-firmware-version", default="0.6.0",
                        help="BL616CL version required by the final scan restoration")
    parser.add_argument("--discovery-snapshots", action="store_true",
                        help="Also pause after successful discovery to collect comparison snapshots")
    parser.add_argument("--winrt-failure-diagnostics", action="store_true",
                        help="Read this WinRT client's numeric state before discovery failure cleanup")
    parser.add_argument("--no-discovery-retry-control", action="store_true",
                        help="Abort on the first SERVICES Unreachable before backend retries")
    parser.add_argument("--services-changed-control", action="store_true",
                        help="Opt-in Bleak WinRT refresh after ServicesChanged; separate from strict discovery")
    parser.add_argument("--release-session-control", action="store_true",
                        help="Control: release WinRT maintain_connection before original disconnect cleanup")
    parser.add_argument("--service-cache-control", action="store_true",
                        help="Control: cache only initial service list; keep live characteristics/data and no retries")
    parser.add_argument("--independent-failure-probe-image", type=Path,
                        help="Preload a read-only central, trigger on discovery failure, then restore baseline RA4")
    parser.add_argument("--winrt-session-diagnostics", action="store_true",
                        help="Record Windows GATT session status enums without device IDs")
    parser.add_argument("--wait-active-control", action="store_true",
                        help="Control: await WinRT ACTIVE before full service enumeration")
    parser.add_argument("--stable-active-control", action="store_true",
                        help="Control: require 1000 ms ACTIVE without session events before discovery")
    parser.add_argument("--winrt-address-type", choices=("public", "random"),
                        help="Control: select an explicit WinRT Bluetooth address type")
    parser.add_argument("--heap-diagnostics", action="store_true",
                        help="record TLS heap diagnostics at BLE and TLS lifecycle boundaries")
    parser.add_argument("--source-only", action="store_true")
    parser.add_argument("--credential-env", type=Path, default=DEFAULT_CREDENTIAL_ENV)
    parser.add_argument("--skip-peer-firewall", action="store_true")
    parser.add_argument("--tls-port", type=int, default=18443)
    parser.add_argument("--cycle-hold-seconds", type=int, default=0)
    parser.add_argument("--cycles", type=int, default=32)
    parser.add_argument("--sessions", type=int, default=4)
    args = parser.parse_args()
    if args.no_discovery_retry_control and (not args.discovery_only
            or not args.winrt_failure_diagnostics or args.backend_retry_compatibility
            or args.services_changed_control or args.service_cache_control
            or args.targeted_service_discovery):
        parser.error("no-retry control needs full discovery-only and failure diagnostics")
    if not re.fullmatch(r"[A-Za-z0-9_-]+", args.windows_ble_venv_name):
        parser.error("Windows BLE environment must be a plain TEMP directory name")
    if args.backend_retry_compatibility and (not args.connected_coex or not args.winrt_session_diagnostics):
        parser.error("Backend compatibility requires connected coexistence and WinRT retry diagnostics")
    if args.independent_failure_probe_image and (
            args.esp32_baseline or not args.connected_coex or args.tls_coex
            or not args.independent_failure_probe_image.is_file()):
        parser.error("failure probe needs an existing probe image and dedicated non-TLS connected coexistence")
    if args.esp32_baseline and (args.tls_coex or not (args.discovery_only or args.connected_coex)):
        parser.error("ESP32 baseline needs discovery-only or scan coexistence, without TLS")
    if args.no_wifi_scan and (not args.connected_coex or args.tls_coex or args.discovery_only):
        parser.error("--no-wifi-scan needs scan coexistence without TLS/discovery-only")
    if args.discovery_only and (args.connected_coex or args.tls_coex or args.mode != "long-gatt"
                                or not 1 <= args.sessions <= 200):
        parser.error("discovery-only needs long-gatt, 1..200 sessions and no coex mode")
    if args.tls_coex:
        args.connected_coex = True
    if not 0 <= args.cycle_hold_seconds <= 60 or (args.cycle_hold_seconds and (
            not args.connected_coex or args.discovery_only or args.mode != "long-gatt")):
        parser.error("cycle hold needs connected coexistence and 0..60 seconds")
    if args.sta_after_connect and not args.tls_coex:
        parser.error("--sta-after-connect needs --tls-coex")
    if args.hci_diagnostics and not args.connected_coex:
        parser.error("--hci-diagnostics needs connected coexistence")
    if args.hci_credit_only and not args.hci_diagnostics:
        parser.error("--hci-credit-only needs --hci-diagnostics")
    if args.hci_snapshot and not (args.connected_coex or args.discovery_only):
        parser.error("HCI snapshot needs connected coexistence or discovery-only")
    if args.discovery_snapshots and not args.hci_snapshot:
        parser.error("discovery snapshots need --hci-snapshot")
    if args.expected_local_cs2 is not None and not args.hci_snapshot:
        parser.error("expected CSA2 feature needs --hci-snapshot")
    if args.targeted_service_discovery and not args.connected_coex:
        parser.error("targeted discovery needs connected coexistence")
    if args.heap_diagnostics and not args.tls_coex:
        parser.error("--heap-diagnostics needs --tls-coex")
    mode = args.mode
    if args.connected_coex and (mode != "long-gatt" or not 1 <= args.cycles <= 200
                               or args.sessions < 1 or args.cycles % args.sessions != 0):
        parser.error("connected coexistence needs long-gatt and cycles divisible by sessions")
    if not 1 <= args.tls_port <= 65535:
        parser.error("TLS port must be 1..65535")
    (ROOT / "build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ble-connected-gate-", dir=ROOT / "build") as temporary:
        return run_gate(args, Path(temporary))


def run_gate(args, work):
    mode = args.mode
    sketch_name = "ra4-arduino-ble-" + mode + "-peripheral"
    port = None
    if not args.source_only:
        if args.esp32_baseline:
            port = resolve_baseline_esp32_port("auto")
            esp32_baseline.require_port_identity(port)
        else:
            port = resolve_paired_bl616cl_port("auto")
            require_final_carrier_bridge(port)
    suffix = "-connected-tls-coex" if args.tls_coex else ("-connected-coex" if args.connected_coex else "")
    if args.discovery_only:
        suffix = "-discovery-" + ("esp32" if args.esp32_baseline else "bl616cl")
    elif args.esp32_baseline:
        suffix += "-esp32"
    if args.no_wifi_scan:
        suffix += "-no-scan"
    build = ROOT / ("build/ra4-ble-windows-" + mode + suffix)
    sketch = ROOT / "examples" / sketch_name
    compile_extra = []
    signals = None
    start_command = b"S"
    tls_host = "192.0.2.1"
    certificate = key = None
    tls_events = work / "windows_tls_dns_peer.events"
    if args.discovery_only:
        signals = work / "signals"
        signals.mkdir()
        compile_extra = ["--build-property", "compiler.cpp.extra_flags=-DBLE_DISCOVERY_GATE"]
        if args.hci_snapshot:
            compile_extra[1] += " -DARDUINO_BLE_COEX_SNAPSHOT"
    if args.connected_coex:
        signals = work / "signals"
        signals.mkdir()
        overlay = signals / "WiFiS3"
        run(sys.executable, str(ROOT / "tools/prepare_wifis3_result_error_overlay.py"),
            "--output-library", str(overlay))
        compile_extra = ["--build-property", "compiler.cpp.extra_flags=-DBLE_WIFI_COEX_GATE",
                         "--library", str(overlay)]
        if args.esp32_baseline:
            compile_extra[1] += " -DBLE_COEX_BASELINE -DBLE_DISCOVERY_GATE"
        if args.no_wifi_scan:
            compile_extra[1] += " -DBLE_COEX_NO_WIFI_SCAN"
        if args.hci_diagnostics:
            compile_extra[1] += " -DARDUINO_BLE_COEX_DIAGNOSTICS"
        if args.hci_credit_only:
            compile_extra[1] += " -DARDUINO_BLE_COEX_CREDIT_ONLY"
        if args.hci_snapshot:
            compile_extra[1] += " -DARDUINO_BLE_COEX_SNAPSHOT"
        if args.heap_diagnostics:
            compile_extra[1] += " -DBLE_TLS_HEAP_DIAGNOSTICS"
    if args.tls_coex:
        if not args.source_only:
            ssid, password = load_private_credentials(args.credential_env)
            if password is None:
                raise RuntimeError("TLS coexistence needs a private WPA password")
            tls_host = tls_gate.windows_wlan_address()
            start_command = ("START " + encode_start_value(ssid) + " " + encode_start_value(password)
                             + " " + tls_host + " " + str(args.tls_port) + "\n").encode("ascii")
            if len(start_command) > 255:
                raise RuntimeError("TLS start command exceeds board buffer")
        ca, certificate, key = tls_gate.create_material(work, "ec", ipaddress.IPv4Address(tls_host))
        sketch = work / sketch_name
        sketch.mkdir()
        shutil.copy2(ROOT / "examples" / sketch_name / (sketch_name + ".ino"), sketch)
        (sketch / "tls_coex_config.h").write_text(
            "#pragma once\nconst char kTlsCoexCa[] =\n"
            + tls_gate.pem_c_literal(ca.read_text(encoding="ascii")) + ";\n", encoding="ascii")
        compile_extra[1] += " -DBLE_TLS_COEX_GATE"
        if args.sta_after_connect:
            compile_extra[1] += " -DBLE_TLS_STA_AFTER_CONNECT"
    run(CLI, "compile", *compile_extra, "--fqbn", "arduino:renesas_uno:unor4wifi",
        "--library", str(ROOT / "libraries/ArduinoBLE"), "--build-path", str(build),
        str(sketch))
    if args.source_only:
        print("RA4_BLE_WINDOWS_SOURCE_COMPILE_PASS" + suffix, flush=True)
        return
    peer = None
    tls_peer = None
    firewall_rule = None
    peer_pid_file = work / "powershell.pid"
    modified = False
    prepared_probe_port = None
    try:
        if args.independent_failure_probe_image:
            from ble_independent_failure_probe import prepare_probe
            prepared_probe_port = prepare_probe(args.independent_failure_probe_image)
        if args.tls_coex:
            if not args.skip_peer_firewall:
                firewall_rule = tls_gate.configure_peer_firewall(tls_host, args.tls_port)
            tls_peer = launch_windows_coex_peer(work, tls_host, args.tls_port, certificate, key,
                                               args.cycles, tls_events, handshake_timeout=15.0)
            tls_gate.wait_for_peer_ready(tls_peer, 15)
            print("BLE_TLS_COEX:PEER_READY", flush=True)
        run(CLI, "board", "list")
        if args.esp32_baseline:
            esp32_baseline.require_port_identity(port)
        else:
            require_final_carrier_bridge(port)
        modified = True
        if args.esp32_baseline:
            print("Target: authorized ESP32-S3 baseline RA4M1 Sketch only; bridge firmware unchanged.", flush=True)
            run(CLI, "upload", "--fqbn", "arduino:renesas_uno:unor4wifi",
                "--port", port, "--input-dir", str(build))
        else:
            run(sys.executable, str(ROOT / "tools/run_ra4_bossac_final_carrier.py"),
                "--bridge-port", port, "--image",
                str(build / (sketch_name + ".ino.bin")),
                "--runtime-settle-ms", "1000")
        with open_gate_serial(port, args.esp32_baseline) as link:
            ready_deadline = time.monotonic() + (60 if args.tls_coex else 20)
            heaps = []
            unavailable_heaps = []
            heap_boundaries = []
            start_sent = False
            local_cs2 = []
            while time.monotonic() < ready_deadline:
                line = link.readline().decode("ascii", errors="replace").strip()
                if line:
                    print(line, flush=True)
                if line.startswith("BLE_COEX_HCI:LOCAL_CS2="):
                    local_cs2.append(int(line.split("=", 1)[1]))
                if args.connected_coex and line == "BLE_WIFI_COEX:WAIT_START" and not start_sent:
                    link.write(start_command)
                    link.flush()
                    start_sent = True
                if line.startswith("BLE_WIFI_COEX:HEAP="):
                    heaps.append(tuple(map(int, line.split("=", 1)[1].split(","))))
                if line.startswith("BLE_WIFI_COEX:HEAP_UNAVAILABLE="):
                    unavailable_heaps.append(int(line.split("=", 1)[1]))
                if line.startswith("BLE_TLS_COEX:HEAP_BOUNDARY="):
                    heap_boundaries.append(line)
                if line.endswith(":READY"):
                    break
                if ":RESULT=FAIL:" in line:
                    raise RuntimeError("BLE peripheral failed to initialize")
            else:
                raise RuntimeError("BLE peripheral did not advertise")
            require_controller_cs2(local_cs2, args.expected_local_cs2)
            script = ROOT / "tools/bl616cl_windows_ble_long_gatt_peer.py"
            windows_script = "\\\\wsl.localhost\\Ubuntu-22.04" + str(script).replace("/", "\\")
            signal = ROOT / "build/windows-ble-disconnect.signal"
            signal.unlink(missing_ok=True)
            windows_signal = "\\\\wsl.localhost\\Ubuntu-22.04" + str(signal).replace("/", "\\")
            peer_log = args.peer_log or ROOT / ("build/windows-ble-" + mode + "-peer-20260929.log")
            peer_extra = ""
            if signals is not None:
                windows_signals = "\\\\wsl.localhost\\Ubuntu-22.04" + str(signals).replace("/", "\\")
                peer_extra = f' --coex-signals "{windows_signals}" --cycles {args.cycles} --sessions {args.sessions}'
                if args.targeted_service_discovery:
                    peer_extra += ' --targeted-service-discovery'
                if args.discovery_only:
                    peer_extra = f' --coex-signals "{windows_signals}" --discovery-sessions {args.sessions}'
                if args.discovery_snapshots:
                    peer_extra += ' --snapshot-discovery'
            if args.cycle_hold_seconds:
                peer_extra += f" --cycle-hold-seconds {args.cycle_hold_seconds}"
            if args.winrt_session_diagnostics:
                peer_extra += ' --winrt-session-diagnostics'
            if args.winrt_failure_diagnostics:
                peer_extra += ' --winrt-failure-diagnostics'
            if args.no_discovery_retry_control:
                peer_extra += ' --no-discovery-retry-control'
            if args.services_changed_control:
                peer_extra += ' --services-changed-control'
            if args.release_session_control:
                peer_extra += ' --release-session-control'
            if args.service_cache_control:
                peer_extra += ' --service-cache-control'
            if args.wait_active_control:
                peer_extra += ' --wait-active-control'
            if args.stable_active_control:
                peer_extra += ' --stable-active-control'
            if args.winrt_address_type:
                peer_extra += ' --winrt-address-type ' + args.winrt_address_type
            with peer_log.open("w", encoding="ascii") as output:
                windows_pid_file = "\\\\wsl.localhost\\Ubuntu-22.04" + str(peer_pid_file).replace("/", "\\")
                peer = subprocess.Popen([
                    "powershell.exe", "-NoProfile", "-Command",
                    '$ErrorActionPreference="Stop"; '
                    f'$venv=Join-Path $env:TEMP "{args.windows_ble_venv_name}"; '
                    f'$PID | Set-Content -LiteralPath "{windows_pid_file}"; '
                    f'& "$venv\\Scripts\\python.exe" "{windows_script}" '
                    f'--disconnect-signal "{windows_signal}" --mode {mode}{peer_extra}; exit $LASTEXITCODE',
                ], stdout=output, stderr=subprocess.STDOUT)
                writes = []
                disconnects = 0
                logical_disconnects = 0
                connected = 0
                cycles = []
                skipped_scans = []
                tls_exchanges = []
                tls_addresses = []
                echoes = 0
                snapshots_requested = snapshots_completed = 0
                started = time.monotonic()
                deadline = started + (args.sessions * 45 + 120 if args.discovery_only else
                                     args.cycles * (40 if args.tls_coex else 20) + 120 if args.connected_coex else 120)
                deadline += args.cycles * args.cycle_hold_seconds
                while time.monotonic() < deadline:
                    if args.discovery_snapshots and signals is not None and snapshots_requested == snapshots_completed:
                        next_session = snapshots_requested + 1
                        if (signals / f"connected-{next_session}.signal").exists():
                            print(f"BLE_COEX_HCI:DISCOVERY_SESSION={next_session}", flush=True)
                            link.write(b"D")
                            link.flush()
                            snapshots_requested = next_session
                    line = link.readline().decode("ascii", errors="replace").strip()
                    if line:
                        print(line, flush=True)
                    if line.startswith("BLE_COEX_HCI:SNAPSHOT=") and snapshots_requested > snapshots_completed:
                        snapshots_completed = snapshots_requested
                        (signals / f"discovery-snapshot-{snapshots_completed}.signal").write_text("PASS\n", encoding="ascii")
                    if ":RESULT=FAIL:" in line:
                        raise RuntimeError("BLE peripheral rejected GATT payload")
                    if line.startswith("BLE_TLS_COEX:EXCHANGE_PASS="):
                        tls_exchanges.append(int(line.split("=", 1)[1]))
                    if line.startswith("BLE_TLS_COEX:HEAP_BOUNDARY="):
                        heap_boundaries.append(line)
                    if line.startswith("BLE_TLS_COEX:ADDRESS_API_PASS="):
                        tls_addresses.append(int(line.split("=", 1)[1]))
                    if line.startswith("BLE_WIFI_COEX:SCAN_SKIPPED="):
                        skipped_scans.append(int(line.split("=", 1)[1]))
                    if signals is not None:
                        if line == "BLE_WIFI_COEX:USB_ECHO_REQUEST":
                            link.write(b"E")
                            link.flush()
                            echoes += 1
                        elif line.startswith("BLE_WIFI_COEX:HEAP="):
                            row = tuple(map(int, line.split("=", 1)[1].split(",")))
                            if len(row) != 3 or row[0] != len(heaps):
                                raise RuntimeError("Connected coexistence heap samples out of order")
                            heaps.append(row)
                        elif line.startswith("BLE_WIFI_COEX:HEAP_UNAVAILABLE="):
                            unavailable_heaps.append(int(line.split("=", 1)[1]))
                        elif line.startswith("BLE_WIFI_COEX:CYCLE="):
                            cycle, networks, elapsed = map(int, line.split("=", 1)[1].split(","))
                            if cycle != len(cycles) + 1 or networks < 0 or elapsed <= 0:
                                raise RuntimeError("Connected coexistence cycle invalid")
                            if args.tls_coex and (tls_exchanges != list(range(1, cycle + 1))
                                                  or tls_addresses != list(range(1, cycle + 1))):
                                raise RuntimeError("TLS evidence missing before GATT readback")
                            cycles.append(cycle)
                            (signals / f"cycle-{cycle}.signal").write_text("PASS\n", encoding="ascii")
                    if line.endswith(":CENTRAL_CONNECTED"):
                        connected += 1
                    if "WRITE_RECEIVED:COUNT=" in line:
                        writes.append(int(line.split("COUNT=", 1)[1].split(":", 1)[0]))
                    if ":NOTIFY_SENT=" in line or ":INDICATION_CONFIRMED=" in line:
                        writes.append(int(line.split("=", 1)[1].split(":", 1)[0]))
                    if line.endswith(":CENTRAL_DISCONNECTED"):
                        disconnects += 1
                        if disconnects == 1:
                            signal.write_text("PERIPHERAL_DISCONNECTED\n", encoding="ascii")
                        if signals is not None:
                            if args.backend_retry_compatibility:
                                logical_disconnects = acknowledge_logical_disconnect(signals, logical_disconnects)
                            else:
                                (signals / f"disconnect-{disconnects}.signal").write_text("PASS\n", encoding="ascii")
                    if peer.poll() is not None and peer.returncode != 0:
                        print(peer_log.read_text(encoding="ascii"), flush=True)
                        raise RuntimeError("Windows BLE peer failed")
                    expected_disconnects = args.sessions + 1 if args.connected_coex else (2 if mode == "long-gatt" else 1)
                    if args.discovery_only:
                        expected_disconnects = args.sessions
                    links_complete = (not args.backend_retry_compatibility or
                                      (logical_disconnects == expected_disconnects and connected == disconnects))
                    if peer.poll() is not None and disconnects >= expected_disconnects and links_complete:
                        break
                else:
                    raise RuntimeError("BLE long-GATT or disconnect confirmation timed out")
                expected = list(range(1, args.cycles + 1)) if args.connected_coex else (
                    list(range(1, 9)) if mode == "long-gatt" else [1, 2, 3, 1, 2, 3])
                if args.discovery_only:
                    expected = []
                    if connected != args.sessions or disconnects != args.sessions:
                        raise RuntimeError("Discovery-only connection evidence incomplete")
                if peer.returncode != 0 or writes != expected:
                    raise RuntimeError("BLE host and dedicated peripheral evidence disagree")
                if args.discovery_snapshots and snapshots_completed != expected_disconnects:
                    raise RuntimeError("Discovery snapshots incomplete")
                if args.connected_coex:
                    validate_hold_evidence(peer_log.read_text(encoding="ascii"), args.cycles, args.cycle_hold_seconds)
                    if skipped_scans != (expected if args.no_wifi_scan else []):
                        raise RuntimeError("Wi-Fi scan control evidence disagrees with selected mode")
                    heap_cycles = unavailable_heaps if args.esp32_baseline else [row[0] for row in heaps]
                    if (cycles != expected or echoes != args.cycles
                            or (not args.backend_retry_compatibility and connected != args.sessions + 1)
                            or heap_cycles != list(range(args.cycles + 1))):
                        raise RuntimeError("Connected coexistence evidence incomplete")
                    if args.backend_retry_compatibility:
                        validate_backend_compatibility(peer_log.read_text(encoding="ascii"), args.sessions,
                                                       connected, disconnects, logical_disconnects)
                    if args.tls_coex:
                        if args.heap_diagnostics:
                            validate_heap_boundaries(heap_boundaries, args.cycles)
                            print("BLE_TLS_COEX:HEAP_BOUNDARIES_PASS", flush=True)
                        if tls_exchanges != expected or tls_addresses != expected:
                            raise RuntimeError("Connected TLS coexistence evidence incomplete")
                        tls_gate.wait_for_peer_events(
                            tls_events, (f"TLS_DNS_PEER:EXCHANGE_PASS={args.cycles}\n",), 10)
                        tls_gate.print_peer_markers(public_tls_events(tls_events))
                        print("BLE_TLS_COEX:RESULT=PASS", flush=True)
                    print(f"BLE_WIFI_COEX:HOST_ELAPSED_MS={int((time.monotonic() - started) * 1000)}")
                    for column, name in ((1, "OCRAM"), (2, "WRAM")):
                        if args.esp32_baseline:
                            continue
                        values = [row[column] for row in heaps[1:]]
                        print(f"BLE_WIFI_COEX:{name}_STEADY_RANGE={min(values)},{max(values)}")
            evidence = peer_log.read_text(encoding="ascii")
            print(evidence, flush=True)
            marker = "WINDOWS_BLE_COEX:RESULT=PASS" if args.connected_coex else (
                "WINDOWS_BLE_LONG_GATT:RESULT=PASS" if mode == "long-gatt" else "WINDOWS_BLE_EVENTS:RESULT=PASS")
            if args.discovery_only:
                marker = "WINDOWS_BLE_DISCOVERY:RESULT=PASS"
            if marker not in evidence:
                raise RuntimeError("BLE Windows peer did not pass")
            print("RA4_BLE_WINDOWS_GATE_PASS mode=" + mode + suffix, flush=True)
    except BaseException:
        if args.hci_snapshot and modified:
            try:
                with open_gate_serial(port, args.esp32_baseline) as diagnostic:
                    diagnostic.write(b"D")
                    diagnostic.flush()
                    deadline = time.monotonic() + 3
                    while time.monotonic() < deadline:
                        line = diagnostic.readline().decode("ascii", errors="replace").strip()
                        if line.startswith(("BLE_COEX_HCI:ATT_TRACE=", "BLE_COEX_HCI:LINK_TRACE=",
                                            "BLE_COEX_HCI:RSSI=", "BLE_COEX_HCI:COMMAND_LAST=", "BLE_COEX_HCI:COMMAND_TRACE=")):
                            print(line, flush=True)
                        if line.startswith("BLE_COEX_HCI:SNAPSHOT="):
                            print(line, flush=True)
                            break
                    else:
                        print("BLE_COEX_HCI:SNAPSHOT_UNAVAILABLE", flush=True)
            except Exception as error:
                print("BLE_COEX_HCI:SNAPSHOT_FAILED=" + type(error).__name__, flush=True)
        if (args.independent_failure_probe_image and peer is not None and peer.poll() is not None
                and peer.returncode != 0 and discovery_failure_kind(
                    peer_log.read_text(encoding="ascii", errors="replace"))):
            try:
                from ble_independent_failure_probe import run_probe
                probe_port = prepared_probe_port
                prepared_probe_port = None  # run_probe owns restoration even on failure.
                run_probe(args.independent_failure_probe_image, len(writes), prepared_port=probe_port)
            except Exception as error:
                print("BLE_INDEPENDENT_FAILURE_PROBE:FAILED=" + type(error).__name__, flush=True)
        raise
    finally:
        try:
            if prepared_probe_port is not None:
                from ble_independent_failure_probe import restore_probe
                restore_probe(prepared_probe_port)
            if peer is not None and peer.poll() is None:
                if peer_pid_file.exists():
                    native_pid = peer_pid_file.read_text(encoding="utf-8-sig").strip()
                    if not native_pid.isdecimal():
                        raise RuntimeError("Invalid Windows peer process identity")
                    subprocess.run(["powershell.exe", "-NoProfile", "-Command",
                                    f"taskkill.exe /PID {native_pid} /T /F"], check=False, timeout=15)
                peer.wait(timeout=10)
        finally:
            try:
                if tls_peer is not None:
                    tls_gate.stop_windows_peer(tls_peer, work)
                    if tls_events.exists():
                        destination = build / "windows_tls_peer.events"
                        destination.write_text(public_tls_events(tls_events), encoding="ascii")
                tls_gate.remove_peer_firewall(firewall_rule)
            finally:
                if modified:
                    stop_confirmed = not (args.discovery_only or args.esp32_baseline) or stop_discovery(port, args.esp32_baseline)
                    if args.esp32_baseline:
                        esp32_baseline.require_port_identity(port)
                        esp32_baseline.restore_scan_smoke(argparse.Namespace(
                            arduino_cli=CLI, fqbn="arduino:renesas_uno:unor4wifi", port=port,
                            scan_startup_timeout_ms=100, scan_capture_timeout_ms=45000))
                    else:
                        if args.connected_coex:
                            restore_usb(physical_reset=True)
                        run(sys.executable, str(ROOT / "tools/run_ra4_wifis3_scan_final_carrier.py"),
                            "--arduino-cli", CLI, "--expected-version", args.expected_firmware_version)
                    print("BLE_WINDOWS_SCAN_RESTORE_PASS mode=" + mode, flush=True)
                    if not stop_confirmed:
                        raise RuntimeError("Discovery advertising stop was not confirmed; scan restored")


if __name__ == "__main__":
    main()
