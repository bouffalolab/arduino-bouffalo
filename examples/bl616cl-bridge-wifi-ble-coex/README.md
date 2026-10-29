# BL616CL Wi-Fi + BLE Coexistence Smoke

This standalone DK-board smoke uses the bridge SDK profile and verifies a
minimal Wi-Fi + BLE coexistence path:

1. initializes MTD, EasyFlash, RF, lwIP/FHOST, and the Wi-Fi manager;
2. starts the BLE controller and the on-chip raw HCI interface;
3. starts a Wi-Fi station scan and immediately sends an HCI Reset command;
4. requires both `CODE_WIFI_ON_SCAN_DONE` and the HCI Reset Command Complete
   event for each of three consecutive cycles.

It emits only marker lines and scan counts. It does not print or store SSIDs,
BSSIDs, credentials, or packet payloads. It does not connect to an AP, access
RA4M1, or exercise USB CDC/CMSIS-DAP.

Run:

```bash
./test-bl616cl-bridge-wifi-ble-coex.sh
```

The script verifies that `/dev/ttyACM1` is the dedicated BL616CL CKLink-Lite,
builds the `bouffalo:bl616cl:bl616cl_bridge_smoke` target, writes boot2,
partition, and application segments, sends the documented CKLink reset
commands, and requires:

```text
BL616CL_SMOKE:COEX_STABLE_DONE cycles=3
```

This is a P0 feasibility result only. It does not demonstrate RA4M1
ArduinoBLE HCI transport compatibility, WiFiS3 AT compatibility, real
Wi-Fi/BLE throughput, or USB composite-device coexistence.
