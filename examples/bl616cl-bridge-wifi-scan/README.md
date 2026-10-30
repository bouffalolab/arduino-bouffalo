# BL616CL Bridge Wi-Fi Scan Smoke

This is a standalone BL616CL DK smoke target. It reuses the project's
`BridgeRuntime`, `WifiService`, and native Bouffalo Wi-Fi backend, then
performs one credential-free station scan after the Wi-Fi/BLE runtime reaches
ready state.

It prints only structured markers and the scan-result count. It never prints
SSID, BSSID, passwords, or scan rows. It does not connect to an access point,
change stored Wi-Fi state, exercise the RA4M1 AT UART, or validate UNO R4
carrier wiring.

The `bl616cl_bridge_smoke` FQBN intentionally uses the standalone DK UART0
mapping so CKLink VCOM can capture these markers. It uses the project `bridge`
SDK profile, but it is not the provisional UNO R4 FQBN and does not validate
the final carrier pin map.

Build, flash, reset, and capture the dedicated board with:

```bash
./test-bl616cl-bridge-wifi-scan.sh
```

The script defaults to `/dev/ttyACM1`; use
`BL616CL_PORT=/dev/ttyACMx ./test-bl616cl-bridge-wifi-scan.sh` after checking
the live USB identity. It only accepts the dedicated CKLink-Lite VID/PID
`42bf:b210` and one of the explicit local serial allowlist entries
(`FactoryAIOT_Prog_B2DDDD` or `FactoryAIOT_Prog_B2E0C2`). It uses the CKLink
VCOM reset protocol at 2 Mbaud and requires:

```text
BL616CL_SMOKE:WIFI_SCAN_DONE count=<number>
```

It writes boot2 at `0x0`, the generated BFPT partition table at `0xE000`, and
the application at `0x10000`. It does not perform a whole-chip erase.
