#!/usr/bin/env python3
"""Keep the final-carrier Wi-Fi + BLE + TLS + USB gate end to end."""

import ast
import inspect
import sys
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[1]
TEMPLATE = (
    ROOT_DIR
    / "examples"
    / "ra4-wifi-ble-tls-usb-coex"
    / "ra4-wifi-ble-tls-usb-coex.ino.in"
)
RUNNER = ROOT_DIR / "tools" / "run_ra4_wifi_ble_tls_usb_coex_gate.py"


def require(source: str, text: str, message: str) -> None:
    if text not in source:
        raise RuntimeError(message)


def main() -> int:
    template = TEMPLATE.read_text(encoding="ascii")
    runner = RUNNER.read_text(encoding="ascii")
    for marker in (
        "RA4_COEX:WIFI_CONNECTED",
        "RA4_COEX:BLE_BEGIN_PASS",
        "RA4_COEX:BLE_SCAN_START_PASS",
        "RA4_COEX:BLE_SCAN_ACTIVE_DURING_TLS",
        "RA4_COEX:BLE_SCAN_STOP_BEFORE_TLS",
        "RA4_COEX:TLS_CONNECT_PASS",
        "RA4_COEX:TLS_DIAG=",
        "RA4_COEX:TLS_VERIFY_FLAGS=",
        "RA4_COEX:TLS_ENDPOINT=",
        "RA4_COEX:TLS_HEAP_SNAPSHOT=",
        "RA4_COEX:TLS_READ_PASS",
        "RA4_COEX:USB_ECHO_PASS",
        "RA4_COEX:WIFI_DISCONNECT_PASS",
        "RA4_COEX:ROUND_PASS=",
        "RA4_COEX:RESULT=PASS",
    ):
        require(template, marker, f"coexistence Sketch must emit {marker}")
    require(
        template,
        "client.connect(endpoint, port)",
        "coexistence Sketch must use a real WiFiSSLClient connection",
    )
    require(
        template,
        "AT+SSLCLIENTDIAG=%d",
        "coexistence Sketch must preserve TLS failure diagnostics",
    )
    require(
        template,
        "BLE.scan()",
        "coexistence Sketch must keep ArduinoBLE scanning active",
    )
    require(
        template,
        "BLE.setScanParameters(0x0040, kScanWindow)",
        "reduced-duty gate must use the ArduinoBLE scan configuration API",
    )
    require(
        runner,
        "--stop-scan-before-tls",
        "coexistence runner must retain an explicit scan-stop control experiment",
    )
    require(
        template,
        'strcmp(command, "ECHO") == 0',
        "coexistence Sketch must require a host USB CDC echo",
    )
    require(
        template,
        "BLE.available()",
        "coexistence Sketch must drain real BLE reports during TLS/USB work",
    )
    require(
        template,
        "kMaxRounds",
        "coexistence Sketch must bound repeat-cycle execution",
    )
    require(
        template,
        "disconnect_station()",
        "coexistence Sketch must verify station disconnect before a new round",
    )
    require(
        runner,
        'link.write(b"ECHO\\n")',
        "coexistence runner must answer the live USB CDC request",
    )
    require(
        runner,
        "--rounds",
        "coexistence runner must support repeated runtime cycles",
    )
    require(
        runner,
        "expected_echoes=args.rounds",
        "coexistence runner must require one USB echo for every cycle",
    )
    require(
        runner,
        "EXCHANGE_PASS=\" + str(args.rounds)",
        "coexistence runner must require every TLS peer exchange",
    )
    require(
        runner,
        "run_ra4_bossac_final_carrier.py",
        "coexistence runner must use final-carrier RA4M1 upload",
    )
    require(
        runner,
        "prepare_wifis3_result_error_overlay.py",
        "coexistence runner must use the checked WiFiS3 AT error parser fix",
    )
    require(
        runner,
        "FINAL_CARRIER_RA4_SCAN_RESTORE_PASS",
        "coexistence runner must restore the final-carrier scan Sketch",
    )
    # Bind every runner call to the shared helper's actual signature. This
    # catches required-parameter additions before an expensive hardware run.
    sys.path.insert(0, str(ROOT_DIR / "tools"))
    from run_ra4_wifi_ble_tls_usb_coex_gate import launch_windows_coex_peer
    signature = inspect.signature(launch_windows_coex_peer)
    calls = 0
    for path in (ROOT_DIR / "tools").glob("*.py"):
        for node in ast.walk(ast.parse(path.read_text(encoding="utf-8"))):
            if (isinstance(node, ast.Call) and isinstance(node.func, ast.Name)
                    and node.func.id == "launch_windows_coex_peer"):
                signature.bind(*([None] * len(node.args)),
                               **{keyword.arg: None for keyword in node.keywords})
                calls += 1
    assert calls >= 2
    print("RA4_WIFI_BLE_TLS_USB_COEX_SOURCE_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
