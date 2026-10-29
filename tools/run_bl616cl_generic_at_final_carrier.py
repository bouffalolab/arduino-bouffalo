#!/usr/bin/env python3
"""Validate generic AT responses and RESET through a temporary RA4M1 proxy."""

import argparse
import os
import tempfile
import subprocess
import sys
import time
from pathlib import Path

import serial

from run_ra4_wifis3_scan_final_carrier import require_final_carrier_bridge
from uno_r4_usb_identity import resolve_paired_bl616cl_port
from bl616cl_usb_mapping import ensure_bl_usb_mapping
from run_ra4_wifis3_tls_gate import wsl_unc_path

ROOT = Path(__file__).resolve().parents[1]
CLI = "arduino-cli"
BUILD = ROOT / "build/ra4-generic-at-proxy"


def run(*command):
    subprocess.run(command, check=True)


def restore_usb(physical_reset=False):
    hold = None
    reset = None
    reset_log = None
    reset_output = None
    try:
        if physical_reset:
            # Keep FTDI ownership until normal USB is attached and open, so
            # closing its handle cannot undo the boot strap before recovery.
            run("usbipd.exe", "list")
            BUILD.mkdir(parents=True, exist_ok=True)
            fd, filename = tempfile.mkstemp(prefix="reset-", suffix=".hold", dir=BUILD)
            os.close(fd)
            hold = Path(filename)
            reset_log = hold.with_suffix(".log")
            reset_output = reset_log.open("w")
            reset = subprocess.Popen([
                "powershell.exe", "-NoProfile", "-ExecutionPolicy", "Bypass",
                "-File", wsl_unc_path(ROOT / "tools/reset_bl616cl_ftdi_windows.ps1"),
                "-HoldUntilReleasedPath", wsl_unc_path(hold),
            ], stdout=reset_output, stderr=subprocess.STDOUT, text=True)
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            if reset is not None and "BL616CL_FTDI_HOLD_ACTIVE" not in reset_log.read_text():
                if reset.poll() is not None:
                    raise RuntimeError("FTDI helper exited before reset/hold became ready")
                time.sleep(0.1)
                continue
            try:
                port = resolve_paired_bl616cl_port("auto")
                require_final_carrier_bridge(port)
                fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
                os.close(fd)
                print("GENERIC_AT_USB_IDENTITY_OPEN_PASS", flush=True)
                return
            except (RuntimeError, OSError):
                ensure_bl_usb_mapping() # resolve current dedicated serial/bus
                time.sleep(0.25)
        raise RuntimeError("BL616CL normal USB did not return")
    finally:
        if hold is not None:
            hold.unlink(missing_ok=True)
        if reset is not None:
            try:
                reset.communicate(timeout=8)
                print(reset_log.read_text(), end="", flush=True)
                if reset.returncode:
                    raise RuntimeError("dedicated BL616CL FTDI reset failed")
            finally:
                reset_output.close()
                reset_log.unlink(missing_ok=True)


def exchange(command, expected):
    port = resolve_paired_bl616cl_port("auto")
    require_final_carrier_bridge(port)
    received = bytearray()
    with serial.Serial(port, 115200, timeout=0.1, write_timeout=2) as link:
        link.dtr = False
        link.rts = False
        link.reset_input_buffer()
        link.write(command)
        link.flush()
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                received.extend(link.read(1))
            except serial.SerialException:
                break
            if received.endswith(b"OK\r\n") or received.endswith(b"ERROR\r\n"):
                break
    if received != expected:
        raise RuntimeError(f"unexpected response to {command!r}: {bytes(received)!r}")
    print("GENERIC_AT_EXACT_PASS=" + command.decode("ascii").strip(), flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--reset-cycles", type=int, default=3)
    parser.add_argument("--skip-upload", action="store_true")
    parser.add_argument("--loopback", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.reset_cycles <= 20:
        raise ValueError("reset cycles must be 1..20")
    port = resolve_paired_bl616cl_port("auto")
    require_final_carrier_bridge(port)
    modified = False
    try:
        run(CLI, "board", "list")
        modified = True
        if not args.skip_upload:
            run(CLI, "compile", "--fqbn", "arduino:renesas_uno:unor4wifi",
                "--build-path", str(BUILD), str(ROOT / "examples/ra4-at-transcript-proxy"))
            run(sys.executable, str(ROOT / "tools/run_ra4_bossac_final_carrier.py"),
                "--bridge-port", port, "--image",
                str(BUILD / "ra4-at-transcript-proxy.ino.bin"), "--runtime-settle-ms", "2000")
        for command, expected in (
            (b"AT\r\n", b"OK\r\n"),
            (b"AT+EXIT\r\n", b"OK\r\n"),
            (b"AT+EXIT?\r\n", b"OK\r\n"),
            (b"AT+EXIT=1\r\n", b"OK\r\n"),
            (b"AT+RESET?\r\n", b"ERROR\r\n"),
            (b"AT+RESTARTBOOTLOADER?\r\n", b"ERROR\r\n"),
            (b"AT+CLIENTSTATUS=8\r\n", b"ERROR\r\n"),
        ):
            exchange(command, expected)
        for command, expected in (
            (b"AT+BEGINCLIENT\r\n", b"+BEGINCLIENT: 0\r\nOK\r\n"),
            (b"AT+IPCLIENT=0\r\n", b"OK\r\n"),
            (b"AT+CLIENTSTATE=0\r\n", b"OK\r\n"),
            (b"AT+CLIENTSTATUS=0\r\n", b"+CLIENTSTATUS: \r\nOK\r\n"),
            (b"AT+CLIENTCLOSE=0\r\n", b"+CLIENTCLOSE: \r\nOK\r\n"),
            (b"AT+SSLBEGINCLIENT\r\n", b"+SSLBEGINCLIENT: 2000\r\nOK\r\n"),
            (b"AT+SSLIPCLIENT=2000\r\n", b"OK\r\n"),
            (b"AT+SSLCLIENTSTATE=2000\r\n", b"OK\r\n"),
            (b"AT+SSLCLIENTSTATUS=2000\r\n", b"+SSLCLIENTSTATUS: \r\nOK\r\n"),
            (b"AT+SSLCLIENTCLOSE=2000\r\n", b"+SSLCLIENTCLOSE: \r\nOK\r\n"),
        ):
            exchange(command, expected)
        if args.loopback:
            for command, expected in (
                (b"AT+BEGINSERVER=18457\r\n", b"+BEGINSERVER: 0\r\nOK\r\n"),
                (b"AT+BEGINCLIENT\r\n", b"+BEGINCLIENT: 0\r\nOK\r\n"),
                (b"AT+IPCLIENT=0\r\n", b"OK\r\n"),
                (b"AT+CLIENTCONNECTIP=0,127.0.0.1,18457\r\n",
                 b"+CLIENTCONNECTIP: \r\nOK\r\n"),
                (b"AT+IPCLIENT=0\r\n", b"+IPCLIENT: 127.0.0.1\r\nOK\r\n"),
                (b"AT+CLIENTSEND=0,1\r\nA", b"OK\r\n"),
                (b"AT+SERVERAVAILABLE=0\r\n", b"+SERVERAVAILABLE: 1000\r\nOK\r\n"),
                (b"AT+IPCLIENT=1000\r\n", b"+IPCLIENT: 127.0.0.1\r\nOK\r\n"),
                (b"AT+CLIENTSTATUS=1000\r\n", b"+CLIENTSTATUS: \r\nOK\r\n"),
                (b"AT+CLIENTRECEIVE=1000,1\r\n", b"+CLIENTRECEIVE: 1|AOK\r\n"),
                (b"AT+CLIENTSEND=1000,1\r\nB", b"OK\r\n"),
                (b"AT+CLIENTRECEIVE=0,1\r\n", b"+CLIENTRECEIVE: 1|BOK\r\n"),
                (b"AT+CLIENTCLOSE=1000\r\n", b"+CLIENTCLOSE: \r\nOK\r\n"),
                (b"AT+CLIENTCLOSE=0\r\n", b"+CLIENTCLOSE: \r\nOK\r\n"),
                (b"AT+SERVEREND=0\r\n", b"+SERVEREND: \r\nOK\r\n"),
            ):
                exchange(command, expected)
            print("GENERIC_AT_LOOPBACK_PASS", flush=True)
        for cycle in range(1, args.reset_cycles + 1):
            exchange(b"AT+RESET\r\n", b"+RESET: \r\nOK\r\n")
            time.sleep(2)
            restore_usb()
            time.sleep(2)
            exchange(b"AT+FWVERSION?\r\n", b"+FWVERSION: 0.6.0\r\nOK\r\n")
            print(f"GENERIC_AT_RESET_CYCLE_PASS={cycle}", flush=True)
        exchange(b"AT+RESTARTBOOTLOADER\r\n", b"+RESTARTBOOTLOADER: \r\nOK\r\n")
        time.sleep(2)
        run("powershell.exe", "-NoProfile", "-Command", r'''
$devices=@(Get-PnpDevice -PresentOnly | Where-Object {
    $_.Status -eq 'OK' -and $_.InstanceId -match 'VID_FFFF&PID_FFFF'
})
if ($devices.Count -ne 1) { throw 'Expected one BL616CL BootROM' }
Write-Output 'GENERIC_AT_BOOTROM_ENUMERATION_PASS'
''')
        restore_usb(physical_reset=True)
        time.sleep(2)
        exchange(b"AT\r\n", b"OK\r\n")
        print("BL616CL_GENERIC_AT_FINAL_CARRIER_PASS", flush=True)
    finally:
        if modified:
            try:
                resolve_paired_bl616cl_port("auto")
            except RuntimeError:
                restore_usb(physical_reset=True)
            run(sys.executable, str(ROOT / "tools/run_ra4_wifis3_scan_final_carrier.py"),
                "--arduino-cli", CLI)
            print("GENERIC_AT_SCAN_RESTORE_PASS", flush=True)


if __name__ == "__main__":
    main()
