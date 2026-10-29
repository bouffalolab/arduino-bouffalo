#!/usr/bin/env python3
"""Compare direct AT UART timing with the installed WiFiS3 response parser."""
import argparse
import json
from pathlib import Path
import statistics
import subprocess
import sys
import time

import serial
from bl616cl_usb_mapping import ensure_bl_usb_mapping
from bounded_serial_close import close_serial_bounded
from serial_control_write import write_control
import run_ra4_wifis3_scan_final_carrier as final_scan

ROOT = Path(__file__).resolve().parents[1]


def summarize(output, scan_window=False):
    if 'RA4_AT_TIMING:RESULT=FAIL' in output:
        raise RuntimeError('AT timing device reported failure')
    rows = {}
    for line in output.replace('\r', '').splitlines():
        if not line.startswith('RA4_AT_TIMING:SAMPLE='):
            continue
        values = tuple(map(int, line.split('=', 1)[1].split(',')))
        if len(values) != 8:
            raise RuntimeError('invalid AT timing row width')
        mode, command, iteration, write, first, total, size, ok = values
        key = mode, command, iteration
        if (key in rows or mode not in range(3 if scan_window else 2) or command not in range(3) or
                iteration not in range(20) or ok != 1 or total <= 0 or total >= 10000000 or
                min(write, first, size) < 0):
            raise RuntimeError('invalid, failed or duplicate AT timing sample')
        if (scan_window or mode == 0) and not (0 < first <= total and write <= total and size >= 4):
            raise RuntimeError('invalid direct UART timing boundaries')
        rows[key] = values
    if len(rows) != (180 if scan_window else 120) or output.replace('\r', '').splitlines().count('RA4_AT_TIMING:RESULT=PASS') != 1:
        raise RuntimeError('incomplete AT timing capture')
    result = []
    for mode in range(3 if scan_window else 2):
        for command in range(3):
            group = [rows[mode, command, i] for i in range(20)]
            item = dict(mode=(('scan-off', 'scan-on', 'scan-off-after') if scan_window else ('raw', 'modem'))[mode],
                        command=(('AT', 'SSLAVAILABLE-unused-slot', 'SSLPEEK-unused-slot') if scan_window else ('AT', 'FWVERSION', 'GETSTATUS'))[command], samples=20)
            for field, index in [('total', 5)] + ([] if mode and not scan_window else [('write', 3), ('first', 4)]):
                values = [row[index] for row in group]
                item[field + '_us'] = dict(min=min(values), median=statistics.median(values), max=max(values))
            result.append(item)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--arduino-cli', default='arduino-cli')
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--scan-window', action='store_true', help='Compare raw AT controls and unused TLS slots with BLE scan off/on/off.')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    if any((build / name).exists() for name in ('capture.log', 'failure-capture.bin', 'summary.json')):
        raise RuntimeError('choose a new build directory to preserve previous timing evidence')
    build.mkdir(parents=True, exist_ok=True)
    def run(command):
        subprocess.run(command, check=True)
    # Inspect actual identity before replacing the dedicated RA4 application.
    port = ensure_bl_usb_mapping()
    final_scan.require_final_carrier_bridge(port)
    run([args.arduino_cli, 'board', 'list'])
    sketch = ROOT / 'examples/ra4-at-roundtrip-timing'
    compile_command = [args.arduino_cli, 'compile', '--fqbn', 'arduino:renesas_uno:unor4wifi',
                       '--build-path', str(build)]
    if args.scan_window:
        compile_command += ['--library', str(ROOT / 'libraries/ArduinoBLE'),
                            '--build-property', 'compiler.cpp.extra_flags=-DAT_TIMING_SCAN_WINDOW=1']
    run(compile_command + [str(sketch)])
    link = None
    data = bytearray()
    modified = False
    error = None
    try:
        port = ensure_bl_usb_mapping()
        print('Target: dedicated RA4M1 timing Sketch; BL616CL Flash unchanged.', flush=True)
        modified = True
        run([sys.executable, str(ROOT / 'tools/run_ra4_bossac_final_carrier.py'),
             '--bridge-port', port, '--image', str(build / 'ra4-at-roundtrip-timing.ino.bin'),
             '--runtime-settle-ms', '500', '--attempts', '3'])
        link = serial.Serial(port, 115200, timeout=0.1, write_timeout=1)
        link.dtr = False
        link.rts = False
        data = bytearray()
        deadline = time.monotonic() + 15
        while b'RA4_AT_TIMING:READY' not in data:
            if time.monotonic() >= deadline:
                raise RuntimeError('AT timing READY timeout')
            data.extend(link.read(512))
        write_control(link, b'S', phase='at-roundtrip-start')
        data.clear()
        deadline = time.monotonic() + 40
        while b'RA4_AT_TIMING:RESULT=' not in data:
            if time.monotonic() >= deadline or len(data) > 32768:
                raise RuntimeError('AT timing capture timeout or oversized result')
            data.extend(link.read(512))
        # read() may end mid final line; collect the newline before validation.
        while not data.endswith(b'\n'):
            if time.monotonic() >= deadline:
                raise RuntimeError('AT timing incomplete terminal line')
            data.extend(link.read(512))
        output = data.decode('ascii', errors='strict')
        (build / 'capture.log').write_text(output)
        summary = summarize(output, args.scan_window)
        (build / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
        print(json.dumps(summary, indent=2), flush=True)
    except Exception as caught:
        error = caught
        if data:
            (build / 'failure-capture.bin').write_bytes(data)
    finally:
        if link is not None:
            try:
                close_serial_bounded(link, phase='at-roundtrip-cleanup')
            except RuntimeError as caught:
                if error is None: error = caught
        if modified:
            try:
                port = ensure_bl_usb_mapping()
                run([sys.executable, str(final_scan.__file__), '--bridge-port', port,
                     '--arduino-cli', args.arduino_cli])
            except Exception as caught:
                if error is None: error = caught
    if error is not None:
        raise error
    print('RA4_AT_ROUNDTRIP_TIMING_PASS', flush=True)


if __name__ == '__main__':
    main()
