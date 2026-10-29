#!/usr/bin/env python3
"""Run Wi-Fi/BLE scan and USB cycles without associating with an upstream AP."""

import argparse
import re
import subprocess
import sys
import tempfile
import time
from pathlib import Path

import serial

from run_bl616cl_generic_at_final_carrier import CLI, ROOT, restore_usb, run
from run_ra4_wifis3_scan_final_carrier import require_final_carrier_bridge
from uno_r4_usb_identity import resolve_paired_bl616cl_port

BUILD = ROOT / "build/ra4-wifi-ble-local-stress"


def capture(port, cycles_required, minimum_duration_seconds):
    heaps = []
    cycles = []
    echoes = 0
    reports = 0
    started = False
    elapsed_ms = None
    with serial.Serial(port, 115200, timeout=0.2, write_timeout=2) as link:
        link.dtr = False
        link.rts = False
        deadline = time.monotonic() + max(600, cycles_required * 20 + 120)
        last_progress = time.monotonic()
        while time.monotonic() < deadline:
            line = link.readline().decode("ascii", errors="replace").strip()
            if not line:
                if started and time.monotonic() - last_progress > 60:
                    raise RuntimeError("local coexistence cycle stalled for 60 seconds")
                continue
            print(line, flush=True)
            last_progress = time.monotonic()
            if line == "LOCAL_COEX:READY" and not started:
                link.write(b"S")
                link.flush()
                started = True
            elif line == "LOCAL_COEX:USB_ECHO_REQUEST":
                link.write(b"E")
                link.flush()
                echoes += 1
            elif line.startswith("LOCAL_COEX:HEAP="):
                row = tuple(map(int, line.split("=", 1)[1].split(",")))
                if len(row) != 3 or row[0] != len(heaps):
                    raise RuntimeError("heap samples missing or out of order")
                heaps.append(row)
            elif line.startswith("LOCAL_COEX:CYCLE="):
                cycle, networks, observed = map(int, line.split("=", 1)[1].split(","))
                if cycle != len(cycles) + 1 or networks < 0 or observed < 0:
                    raise RuntimeError("coexistence cycle invalid or out of order")
                cycles.append(cycle)
                reports += observed
            elif line.startswith("LOCAL_COEX:ELAPSED_MS="):
                elapsed_ms = int(line.split("=", 1)[1])
            elif line == "LOCAL_COEX:RESULT=FAIL":
                raise RuntimeError("local coexistence Sketch failed")
            elif line == "LOCAL_COEX:RESULT=PASS":
                if cycles != list(range(1, cycles_required + 1)) or echoes != cycles_required:
                    raise RuntimeError("local coexistence cycles or USB echoes incomplete")
                if [row[0] for row in heaps] != list(range(cycles_required + 1)) or reports == 0:
                    raise RuntimeError("heap observations or BLE reports incomplete")
                if elapsed_ms is None or elapsed_ms < minimum_duration_seconds * 1000:
                    raise RuntimeError("local coexistence duration below required minimum")
                for column, name in ((1, "OCRAM"), (2, "WRAM")):
                    values = [row[column] for row in heaps]
                    print(f"LOCAL_COEX:{name}_RANGE={min(values)},{max(values)}")
                    print(f"LOCAL_COEX:{name}_BASELINE_FINAL_LOSS={values[0] - values[-1]}")
                print("RA4_WIFI_BLE_LOCAL_STRESS_PASS", flush=True)
                return
        raise RuntimeError("local coexistence capture timed out")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--windows-native", action="store_true")
    parser.add_argument("--capture-windows", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--cycles", type=int, default=40)
    parser.add_argument("--minimum-duration-seconds", type=int, default=0)
    args = parser.parse_args()
    if not 1 <= args.cycles <= 1000 or args.minimum_duration_seconds < 0:
        parser.error("cycles must be 1..1000 and minimum duration must be nonnegative")
    if args.capture_windows:
        from serial.tools import list_ports
        deadline = time.monotonic() + 15
        matches = []
        while time.monotonic() < deadline:
            matches = [p for p in list_ports.comports()
                       if p.vid == 0x2341 and p.pid == 0x1002
                       and p.serial_number == "BL616CL-PROVISIONAL"]
            if len(matches) == 1:
                break
            time.sleep(0.5)
        if len(matches) != 1:
            raise RuntimeError("Expected one dedicated BL616CL CDC interface on Windows")
        run("powershell.exe", "-NoProfile", "-Command",
            '$devices=@(Get-PnpDevice -PresentOnly | Where-Object { '
            f'$_.FriendlyName -like "*({matches[0].device})" -and '
            '$_.InstanceId -match "VID_2341&PID_1002&MI_00" }); '
            'if ($devices.Count -ne 1) { throw "BL616CL CDC interface identity mismatch" }')
        print(f"LOCAL_COEX:WINDOWS_TARGET={matches[0].device},{matches[0].hwid}", flush=True)
        capture(matches[0].device, args.cycles, args.minimum_duration_seconds)
        return
    port = resolve_paired_bl616cl_port("auto")
    require_final_carrier_bridge(port)
    overlay = Path(tempfile.mkdtemp(prefix="ra4-local-coex-overlay-",
                                    dir=ROOT / "build")) / "WiFiS3"
    run(sys.executable, str(ROOT / "tools/prepare_wifis3_result_error_overlay.py"),
        "--output-library", str(overlay))
    run(CLI, "compile", "--fqbn", "arduino:renesas_uno:unor4wifi",
        "--build-path", str(BUILD), "--library", str(overlay),
        "--build-property", f"compiler.cpp.extra_flags=-DLOCAL_COEX_CYCLES={args.cycles}",
        "--library", str(ROOT / "libraries/ArduinoBLE"),
        str(ROOT / "examples/ra4-wifi-ble-local-stress"))
    modified = False
    passed = False
    try:
        run(CLI, "board", "list")
        require_final_carrier_bridge(port)
        modified = True
        run(sys.executable, str(ROOT / "tools/run_ra4_bossac_final_carrier.py"),
            "--bridge-port", port, "--image", str(BUILD / "ra4-wifi-ble-local-stress.ino.bin"),
            "--runtime-settle-ms", "1000")
        if args.windows_native:
            usbip = "/usr/lib/linux-tools-5.15.0-194/usbip"
            enumeration = subprocess.check_output(
                ["wsl.exe", "-d", "Ubuntu-22.04", "-u", "root", "--",
                 usbip, "port"], text=True)
            matching_ports = re.findall(
                r"Port (\d+):(?:(?!Port \d+:).)*usbip://172\.31\.224\.1:3240/4-1",
                enumeration, flags=re.DOTALL)
            if len(matching_ports) != 1:
                raise RuntimeError("Cannot identify dedicated BL616CL USB/IP import")
            require_final_carrier_bridge(port)
            run("wsl.exe", "-d", "Ubuntu-22.04", "-u", "root", "--",
                usbip, "detach", "--port=" + matching_ports[0])
            script = "\\\\wsl.localhost\\Ubuntu-22.04" + str(Path(__file__).resolve()).replace("/", "\\")
            run("powershell.exe", "-NoProfile", "-Command",
                '$ErrorActionPreference="Stop"; $venv=Join-Path $env:TEMP "uno-r4-ble-host-venv"; '
                f'& "$venv\\Scripts\\python.exe" "{script}" --capture-windows '
                f'--cycles {args.cycles} --minimum-duration-seconds {args.minimum_duration_seconds}; '
                'exit $LASTEXITCODE')
            restore_usb()
        else:
            capture(port, args.cycles, args.minimum_duration_seconds)
        passed = True
    finally:
        if modified:
            if not passed:
                restore_usb(physical_reset=True)
            run(sys.executable, str(ROOT / "tools/run_ra4_wifis3_scan_final_carrier.py"),
                "--arduino-cli", CLI)
            print("LOCAL_COEX_SCAN_RESTORE_PASS", flush=True)


if __name__ == "__main__":
    main()
