#!/usr/bin/env python3
"""Reset the authorized original bridge through AT, then restore its RA4 scan."""
import subprocess
import time

from ble_independent_failure_probe import prepare_baseline_usb, restore_probe
from run_bl616cl_generic_at_final_carrier import CLI, ROOT
import run_esp32_sta_baseline_gate as baseline
from uno_r4_usb_identity import resolve_baseline_esp32_port, tty_properties


def require_at(port):
    baseline.require_port_identity(port)
    with baseline.open_serial(port, .2) as link:
        link.write_timeout = 2
        link.reset_input_buffer()
        link.write(b"AT\r\n")
        link.flush()
        response = bytearray()
        deadline = time.monotonic() + 3
        while time.monotonic() < deadline and not response.endswith(b"OK\r\n"):
            response.extend(link.read(1))
        if not response.endswith(b"OK\r\n"):
            raise RuntimeError("Baseline AT check failed")


def main():
    prepare_baseline_usb()
    port = resolve_baseline_esp32_port("auto")
    baseline.require_port_identity(port)
    print("ESP32_RUNTIME_RESET:TARGET=" + tty_properties(port)["ID_SERIAL_SHORT"], flush=True)
    print("ESP32_RUNTIME_RESET:AT RESET; temporary RA4M1 proxy; bridge Flash untouched", flush=True)
    sketch = ROOT / "examples/ra4-at-transcript-proxy"
    build = ROOT / "build/esp32-runtime-reset-proxy"
    subprocess.run([CLI, "compile", "--fqbn", "arduino:renesas_uno:unor4wifi",
                    "--build-path", str(build), str(sketch)], check=True, timeout=90)
    modified = False
    try:
        modified = True
        baseline.require_port_identity(port)
        subprocess.run([CLI, "upload", "--fqbn", "arduino:renesas_uno:unor4wifi",
                        "--port", port, "--input-dir", str(build), str(sketch)],
                       check=True, timeout=90)
        require_at(port)
        print("ESP32_RUNTIME_RESET:PRE_RESET_AT_PASS", flush=True)
        with baseline.open_serial(port, .2) as link:
            link.write_timeout = 2
            link.write(b"AT+RESET\r\n")
            link.flush()
        print("ESP32_RUNTIME_RESET:RESET_SENT", flush=True)
        time.sleep(2)
        prepare_baseline_usb()
        port = resolve_baseline_esp32_port("auto")
        require_at(port)
        print("ESP32_RUNTIME_RESET:POST_RESET_AT_PASS", flush=True)
    finally:
        if modified:
            prepare_baseline_usb()
            restore_probe(resolve_baseline_esp32_port("auto"))


if __name__ == "__main__":
    main()
