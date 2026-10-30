# Standalone BL616CL BLE HCI Smoke

This smoke runs on the dedicated standalone BL616CL DK board through the
`bouffalo:bl616cl:bl616cl_bridge_smoke` FQBN. It links the same `bridge` SDK
profile used by the provisional UNO R4 bridge, while retaining the DK UART0
mapping only for CKLink VCOM logs.

The test exercises the project `BridgeRuntime` and HCI adapter, not a
separate hand-written SDK transaction:

1. Initialize MTD, EasyFlash, RF parameters, and Wi-Fi coexistence services
   through the same `BridgeRuntime` used by `UNOR4BL616CLBridge`.
2. Initialize the BLE controller through `BouffaloHciController` only after
   those runtime prerequisites report ready.
3. Attach `BleHciService`, send the H4 Reset command `01 03 0C 00`, and
   require the reconstructed H4 Command Complete event
   `04 0E 04 01 03 0C 00`.
4. Call `BleHciService::end()`, reattach it with `begin()` without recreating
   the controller/on-chip task, then repeat the same Reset exchange.

It does not start a high-level BLE host, advertise, scan, pair, connect,
persist data, use Wi-Fi, or access RA4M1. Passing this smoke proves only the
runtime/bootstrap and adapter's minimum controller/raw-HCI lifecycle on the
standalone DK board; it does not prove RA4M1 AT transport or ArduinoBLE
end-to-end compatibility.

Run `./test-bl616cl-bridge-ble-hci.sh`. If USB enumeration assigns the
dedicated CKLink-Lite VCOM a different node, pass that node explicitly, for
example `BL616CL_PORT=/dev/ttyACM2 ./test-bl616cl-bridge-ble-hci.sh`. The
script refuses any serial device other than the recorded dedicated BL616CL
CKLink-Lite identity. It writes the Arduino build artifacts at the
SDK-defined addresses: boot2 at `0x0`, the BFPT partition table at `0xE000`,
and the application at `0x10000`; it does not perform a whole-chip erase.

The bridge linker profile reserves the SDK-required 16 KiB BLE EM region. This
is required even for the controller-only transaction because the controller
archive is part of the Wi-Fi/BLE coexistence runtime.
