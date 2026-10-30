"""Read the surviving BL peripheral from the authorized independent central."""
import argparse
import json
import re
import subprocess
from pathlib import Path
import time

import run_esp32_sta_baseline_gate as baseline
from run_bl616cl_generic_at_final_carrier import CLI, ROOT, run
from uno_r4_usb_identity import BASELINE_ESP32_SERIALS, resolve_baseline_esp32_port


def prepare_baseline_usb(sys_root=Path("/sys")):
    """Repair only a Windows-confirmed detached, authorized baseline device."""
    result = subprocess.run(["usbipd.exe", "state"], capture_output=True, check=True, timeout=10)
    devices = json.loads(result.stdout.decode("utf-8-sig"))["Devices"]
    targets = [device for device in devices if device.get("BusId") and
               device.get("InstanceId", "").upper() in
               {"USB\\VID_2341&PID_1002\\" + serial for serial in BASELINE_ESP32_SERIALS}]
    if len(targets) != 1:
        raise RuntimeError("Expected exactly one authorized Windows baseline device")
    target = targets[0]
    if target.get("IsWslAttached"):
        return
    bus_id = target["BusId"]
    if not re.fullmatch(r"\d+-[\d.]+", bus_id):
        raise RuntimeError("Invalid baseline USB bus identity")
    expected_serial = target["InstanceId"].rsplit("\\", 1)[-1].upper()
    for controller in sorted((sys_root / "devices/platform").glob("vhci_hcd.*")):
        if not re.fullmatch(r"vhci_hcd\.\d+", controller.name):
            continue
        for row in (controller / "status").read_text().splitlines()[1:]:
            fields = row.split()
            if (len(fields) != 7 or fields[0] != "hs" or fields[2] != "006"
                    or not fields[1].isdigit() or not re.fullmatch(r"\d+-[\d.]+", fields[6])):
                continue
            device_path = sys_root / "bus/usb/devices" / fields[6]
            try:
                identity = tuple((device_path / name).read_text().strip().upper()
                                 for name in ("idVendor", "idProduct", "serial"))
            except OSError:
                continue
            if identity == ("2341", "1002", expected_serial):
                subprocess.run(["wsl.exe", "-d", "Ubuntu-22.04", "-u", "root", "--", "sh", "-c",
                                f"echo {int(fields[1])} > {controller / 'detach'}"],
                               check=True, capture_output=True, timeout=10)
    subprocess.run(["usbipd.exe", "wsl", "attach", "--busid", bus_id,
                    "--distribution", "Ubuntu-22.04"], check=True, capture_output=True, timeout=15)
    print("BLE_INDEPENDENT_FAILURE_PROBE:BASELINE_USB_REATTACHED", flush=True)
    deadline = time.monotonic() + 10
    while True:
        try:
            resolve_baseline_esp32_port("auto")
            return
        except RuntimeError:
            if time.monotonic() >= deadline:
                raise
            time.sleep(.1)


def restore_probe(port):
    baseline.require_port_identity(port)
    baseline.restore_scan_smoke(argparse.Namespace(
        arduino_cli=CLI, fqbn="arduino:renesas_uno:unor4wifi", port=port,
        scan_startup_timeout_ms=100, scan_capture_timeout_ms=45000))
    print("BLE_INDEPENDENT_FAILURE_PROBE:BASELINE_RESTORED", flush=True)


def prepare_probe(image):
    image = Path(image).resolve()
    if not image.is_file():
        raise RuntimeError("Invalid independent failure probe image")
    prepare_baseline_usb()
    port = resolve_baseline_esp32_port("auto")
    baseline.require_port_identity(port)
    run(CLI, "board", "list")
    print("BLE_INDEPENDENT_FAILURE_PROBE:TARGET=authorized ESP32-S3 RA4M1; BL untouched", flush=True)
    try:
        subprocess.run([CLI, "upload", "--fqbn", "arduino:renesas_uno:unor4wifi",
                        "--port", port, "--input-file", str(image),
                        str(ROOT / "examples/ra4-arduino-ble-long-gatt-central")],
                       check=True, timeout=90)
        baseline.require_port_identity(port)
    except BaseException:
        restore_probe(port)
        raise
    print("BLE_INDEPENDENT_FAILURE_PROBE:UPLOADED_WAITING_FOR_TRIGGER", flush=True)
    return port


def run_probe(image, generation, prepared_port=None):
    probe_started = time.monotonic()
    if not Path(image).is_file() or not 0 <= generation <= 200:
        raise RuntimeError("Invalid independent failure probe image/generation")
    port = prepared_port if prepared_port is not None else prepare_probe(image)
    try:
        baseline.require_port_identity(port)
        markers = []
        started = False
        with baseline.open_serial(port, .1) as link:
            link.write_timeout = 2
            deadline = time.monotonic() + 90
            while time.monotonic() < deadline:
                line = link.readline().decode("ascii", errors="replace").strip()
                if not line:
                    continue
                print("INDEPENDENT_RA4:" + line, flush=True)
                if line.startswith("RA4_BLE_LONG_GATT_CENTRAL:"):
                    phase = line.split(":", 1)[1]
                    if phase in ("BLE_BEGIN=PASS", "TARGET_FOUND", "CONNECT=PASS", "DISCOVER=PASS"):
                        print("BLE_INDEPENDENT_FAILURE_PROBE:PHASE_MS=" + phase + "," +
                              str(int((time.monotonic() - probe_started) * 1000)), flush=True)
                if line == "RA4_BLE_LONG_GATT_CENTRAL:WAIT_START" and not started:
                    print("BLE_INDEPENDENT_FAILURE_PROBE:START_AFTER_MS=" +
                          str(int((time.monotonic() - probe_started) * 1000)), flush=True)
                    link.write(b"S" + bytes((generation,))); link.flush()
                    started = True
                if ":RESULT=FAIL:" in line:
                    raise RuntimeError("Independent probe rejected operation")
                for marker in ("CONNECT=PASS", "DISCOVER=PASS", f"PROBE_READ_PASS={generation}",
                               "SESSION_PASS=1", "RESULT=PASS"):
                    if line == "RA4_BLE_LONG_GATT_CENTRAL:" + marker:
                        markers.append(marker)
                if line == "RA4_BLE_LONG_GATT_CENTRAL:RESULT=PASS":
                    if markers != ["CONNECT=PASS", "DISCOVER=PASS", f"PROBE_READ_PASS={generation}",
                                   "SESSION_PASS=1", "RESULT=PASS"]:
                        raise RuntimeError("Independent probe evidence incomplete")
                    print("BLE_INDEPENDENT_FAILURE_PROBE:READ_AFTER_MS=" +
                          str(int((time.monotonic() - probe_started) * 1000)), flush=True)
                    print("BLE_INDEPENDENT_FAILURE_PROBE:READ_PASS", flush=True)
                    break
            else:
                print("BLE_INDEPENDENT_FAILURE_PROBE:DEADLINE_EXPIRED_MS=" +
                      str(int((time.monotonic() - probe_started) * 1000)), flush=True)
                raise RuntimeError("Independent probe deadline expired")
    finally:
        restore_probe(port)
