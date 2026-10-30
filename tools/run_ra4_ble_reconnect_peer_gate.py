#!/usr/bin/env python3
"""Exercise BL peripheral reconnects from the authorized ESP32-S3 central."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys
import tempfile
import time

import serial
import run_esp32_sta_baseline_gate as baseline
from run_bl616cl_generic_at_final_carrier import CLI, ROOT, restore_usb, run
from run_ra4_wifis3_scan_final_carrier import require_final_carrier_bridge
from uno_r4_usb_identity import resolve_baseline_esp32_port, resolve_paired_bl616cl_port


def preserve_inputs(build, destination, args):
    destination.mkdir(parents=True, exist_ok=False)
    files = []
    for target in ("central", "peripheral"):
        stem = f"ra4-arduino-ble-long-gatt-{target}.ino"
        for name in ("build.options.json", stem + ".bin", stem + ".map", "sketch/" + stem + ".cpp"):
            output = destination / target / name
            output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(build / target / name, output)
            files.append(dict(path=str(output.relative_to(destination)),
                              sha256=hashlib.sha256(output.read_bytes()).hexdigest(),
                              bytes=output.stat().st_size))
    (destination / "manifest.json").write_text(json.dumps(dict(
        sessions=args.sessions, iterations=args.iterations,
        wifi_scan=not args.no_wifi_scan, metadata_gate=True,
        scope="Compiled RA4M1 fixtures; hardware result recorded separately", files=files),
        indent=2) + "\n")
    print("BLE_R4_PEER_COMPILED_INPUTS=" + str(destination), flush=True)


def validate(evidence, sessions, cycles, no_wifi_scan=False):
    expected = list(range(1, cycles + 1))
    for key in ("writes", "cycles", "central_cycles"):
        if evidence[key] != expected:
            raise RuntimeError("Incomplete or reordered " + key)
    if evidence["central_sessions"] != list(range(1, sessions + 1)):
        raise RuntimeError("Incomplete central disconnect evidence")
    if evidence["metadata_sessions"] != list(range(1, sessions + 1)):
        raise RuntimeError("Incomplete GAP/GATT metadata evidence")
    if evidence["connected"] != sessions or evidence["disconnected"] != sessions:
        raise RuntimeError("Peripheral connection count disagrees with central")
    if evidence["echoes"] != cycles or [row[0] for row in evidence["heaps"]] != list(range(cycles + 1)):
        raise RuntimeError("USB/heap evidence incomplete")
    if evidence["skipped_scans"] != (expected if no_wifi_scan else []):
        raise RuntimeError("Scan control evidence disagrees with selected mode")
    if any(evidence["heaps"][-1][column] < evidence["heaps"][1][column] for column in (1, 2)):
        raise RuntimeError("Final heap below first active-cycle sample")


class Lines:
    def __init__(self, link):
        self.link = link
        self.buffer = bytearray()

    def read(self):
        self.buffer.extend(self.link.read(self.link.in_waiting or 1))
        if len(self.buffer) > 16384:
            raise RuntimeError("Serial line buffer overflow")
        while b"\n" in self.buffer:
            line, _, rest = self.buffer.partition(b"\n")
            self.buffer = bytearray(rest)
            yield line.decode("ascii", errors="replace").strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sessions", type=int, default=32)
    parser.add_argument("--iterations", type=int, default=2)
    parser.add_argument("--no-wifi-scan", action="store_true")
    parser.add_argument("--source-only", action="store_true")
    args = parser.parse_args()
    cycles = args.sessions * args.iterations
    if args.sessions < 1 or args.iterations < 1 or cycles > 200:
        parser.error("sessions and iterations must be positive with at most 200 cycles")
    (ROOT / "build").mkdir(exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="ble-r4-peer-", dir=ROOT / "build") as temporary:
        work = Path(temporary)
        overlay = work / "WiFiS3"
        run(sys.executable, str(ROOT / "tools/prepare_wifis3_result_error_overlay.py"),
            "--output-library", str(overlay))
        peripheral = ROOT / "examples/ra4-arduino-ble-long-gatt-peripheral"
        central = ROOT / "examples/ra4-arduino-ble-long-gatt-central"
        build = ROOT / "build/ra4-ble-reconnect-peer"
        flags = "-DBLE_WIFI_COEX_GATE -DARDUINO_BLE_COEX_SNAPSHOT -DLONG_GATT_PEER_GATE"
        if args.no_wifi_scan:
            flags += " -DBLE_COEX_NO_WIFI_SCAN"
        for sketch, destination, extra in (
                (peripheral, build / "peripheral", flags),
                (central, build / "central", f"-DARDUINO_BLE_COEX_SNAPSHOT -DLONG_GATT_PEER_GATE -DLONG_GATT_SESSIONS={args.sessions} -DLONG_GATT_ITERATIONS={args.iterations}")):
            run(CLI, "compile", "--fqbn", "arduino:renesas_uno:unor4wifi",
                "--library", str(ROOT / "libraries/ArduinoBLE"), "--library", str(overlay),
                "--build-property", "compiler.cpp.extra_flags=" + extra,
                "--build-path", str(destination), str(sketch))
        if args.source_only:
            print("BLE_R4_PEER_SOURCE_COMPILE_PASS", flush=True)
            return
        preserve_inputs(build, ROOT / "build" / f"ble-independent-peer-inputs-{time.time_ns()}", args)
        run_gate(args, build, cycles)


def run_gate(args, build, cycles):
    bl_port = resolve_paired_bl616cl_port("auto")
    esp_port = resolve_baseline_esp32_port("auto")
    require_final_carrier_bridge(bl_port)
    baseline.require_port_identity(esp_port)
    print(f"Target BL616CL peripheral RA4M1: {bl_port}; authorized ESP32-S3 central RA4M1: {esp_port}", flush=True)
    print("Both RA4M1 Sketches are replaced; bridge firmware Flash is unchanged.", flush=True)
    evidence = {key: [] for key in ("writes", "cycles", "central_cycles", "central_sessions", "metadata_sessions", "heaps", "skipped_scans")}
    evidence.update(connected=0, disconnected=0, echoes=0, start_sent=False)
    modified_bl = modified_esp = False
    bl = esp = None
    error = None
    started = time.monotonic()

    def bl_line(line):
        if not line:
            return
        print("BL_RA4:" + line, flush=True)
        if ":RESULT=FAIL:" in line:
            raise RuntimeError("BL peripheral rejected operation")
        if line == "BLE_WIFI_COEX:WAIT_START":
            if not evidence["start_sent"]:
                bl.write(b"S"); bl.flush()
                evidence["start_sent"] = True
        elif line == "BLE_WIFI_COEX:USB_ECHO_REQUEST":
            bl.write(b"E"); bl.flush()
            evidence["echoes"] += 1
        elif line.startswith("BLE_WIFI_COEX:HEAP="):
            evidence["heaps"].append(tuple(map(int, line.split("=", 1)[1].split(","))))
        elif line.startswith("BLE_WIFI_COEX:CYCLE="):
            cycle, networks, elapsed = map(int, line.split("=", 1)[1].split(","))
            if networks < 0 or elapsed <= 0:
                raise RuntimeError("Invalid peripheral scan result")
            evidence["cycles"].append(cycle)
        elif line.startswith("BLE_WIFI_COEX:SCAN_SKIPPED="):
            evidence["skipped_scans"].append(int(line.split("=", 1)[1]))
        elif "WRITE_RECEIVED:COUNT=" in line:
            evidence["writes"].append(int(line.split("COUNT=", 1)[1].split(":", 1)[0]))
        elif line.endswith(":CENTRAL_CONNECTED"):
            evidence["connected"] += 1
        elif line.endswith(":CENTRAL_DISCONNECTED"):
            evidence["disconnected"] += 1

    try:
        run(CLI, "board", "list")
        require_final_carrier_bridge(bl_port)
        modified_bl = True
        run(sys.executable, str(ROOT / "tools/run_ra4_bossac_final_carrier.py"),
            "--bridge-port", bl_port, "--image", str(build / "peripheral/ra4-arduino-ble-long-gatt-peripheral.ino.bin"),
            "--runtime-settle-ms", "1000")
        bl = serial.Serial(bl_port, 115200, timeout=0.05, write_timeout=2)
        bl.dtr = bl.rts = False
        bl_lines = Lines(bl)
        ready = False
        deadline = time.monotonic() + 30
        while not ready and time.monotonic() < deadline:
            for line in bl_lines.read():
                bl_line(line)
                if line.endswith(":READY"):
                    ready = True
        if not ready:
            raise RuntimeError("BL peripheral did not become ready")
        run(CLI, "board", "list")
        baseline.require_port_identity(esp_port)
        modified_esp = True
        run(CLI, "upload", "--fqbn", "arduino:renesas_uno:unor4wifi",
            "--port", esp_port, "--input-dir", str(build / "central"))
        esp = baseline.open_serial(esp_port, 0.05)
        esp.write_timeout = 2
        esp_lines = Lines(esp)
        passed = False
        central_start_sent = False
        deadline = time.monotonic() + cycles * 25 + 120
        while time.monotonic() < deadline:
            for line in bl_lines.read():
                bl_line(line)
            for line in esp_lines.read():
                if not line:
                    continue
                print("ESP_RA4:" + line, flush=True)
                if line == "RA4_BLE_LONG_GATT_CENTRAL:WAIT_START" and not central_start_sent:
                    esp.write(b"S"); esp.flush()
                    central_start_sent = True
                if ":RESULT=FAIL:" in line:
                    raise RuntimeError("Independent central failed")
                for marker, key in (("CYCLE_PASS=", "central_cycles"), ("SESSION_PASS=", "central_sessions"),
                                    ("METADATA_PASS=", "metadata_sessions")):
                    match = re.fullmatch("RA4_BLE_LONG_GATT_CENTRAL:" + marker + r"(\d+)", line)
                    if match:
                        evidence[key].append(int(match[1]))
                if line == "RA4_BLE_LONG_GATT_CENTRAL:RESULT=PASS":
                    passed = True
            if passed and evidence["disconnected"] >= args.sessions:
                validate(evidence, args.sessions, cycles, args.no_wifi_scan)
                print(f"BLE_R4_PEER_DATA_PASS cycles={cycles} sessions={args.sessions} elapsed_ms={int((time.monotonic()-started)*1000)}", flush=True)
                for column, region in ((1, "OCRAM"), (2, "WRAM")):
                    values = [row[column] for row in evidence["heaps"][1:]]
                    print(f"BLE_R4_PEER_{region}_RANGE={min(values)},{max(values)}", flush=True)
                break
        else:
            raise RuntimeError("Independent peer deadline expired")
    except BaseException as caught:
        error = caught
        if bl is not None and bl.is_open:
            try:
                bl.write(b"D"); bl.flush()
                deadline = time.monotonic() + 4
                while time.monotonic() < deadline:
                    line = bl.readline().decode("ascii", errors="replace").strip()
                    if line.startswith("BLE_COEX_HCI:"):
                        print(line, flush=True)
                    if line.startswith("BLE_COEX_HCI:SNAPSHOT="):
                        break
            except Exception as diagnostic_error:
                print("BLE_R4_PEER_DIAGNOSTIC_FAILED=" + type(diagnostic_error).__name__, flush=True)
    finally:
        for link in (bl, esp):
            if link is not None:
                link.close()
        for target, modified in (("bl", modified_bl), ("esp", modified_esp)):
            if not modified:
                continue
            try:
                if target == "bl":
                    restore_usb(physical_reset=True)
                    run(sys.executable, str(ROOT / "tools/run_ra4_wifis3_scan_final_carrier.py"),
                        "--arduino-cli", CLI)
                else:
                    baseline.require_port_identity(esp_port)
                    baseline.restore_scan_smoke(argparse.Namespace(
                        arduino_cli=CLI, fqbn="arduino:renesas_uno:unor4wifi", port=esp_port,
                        scan_startup_timeout_ms=100, scan_capture_timeout_ms=45000))
                print("BLE_R4_PEER_SCAN_RESTORE_PASS=" + target, flush=True)
            except BaseException as cleanup_error:
                print("BLE_R4_PEER_SCAN_RESTORE_FAILED=" + target, flush=True)
                if error is None:
                    error = cleanup_error
    if error is not None:
        raise error
    print("BLE_R4_PEER_GATE_PASS", flush=True)


if __name__ == "__main__":
    main()
