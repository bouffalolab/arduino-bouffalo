# RA4M1 ArduinoBLE Begin Smoke

This temporary RA4M1 Sketch executes one `BLE.begin()` call through the
production `ArduinoBLE` transport used by the UNO R4 WiFi core. It is a
diagnostic for the ESP32-S3 raw HCI AT path after the direct HCI transcript
observed `HCIAVAILABLE? -> ERROR` and HCI Reset status `0x05`.

It does not scan, advertise, connect, pair, store credentials, print BLE
addresses, or print HCI packets. The serial output contains only fixed
`RA4_BLE_SMOKE` markers, the `BLE.begin()` pass/fail result, and elapsed time.
After initialization, it repeats the cached result every two seconds without
calling `BLE.begin()` again.

## Scope

`BLE.begin()` reaches `BLELocalDevice::begin()`, which calls `HCI.begin()` and
`HCI.reset()` before continuing with the rest of local-controller
initialization. `HCI.reset()` polls the UNO R4 `HCIVirtualTransportAT`, so this
exercise covers the actual `begin()`, `wait()`, `available()`, and `read()`
path used by ArduinoBLE rather than a host-issued AT command sequence.

The Sketch intentionally does not enable HCI debug output: successful later
initialization could include controller-specific data that is not needed for
this compatibility gate.

## Build

```bash
arduino-cli compile \
  --fqbn arduino:renesas_uno:unor4wifi \
  --libraries libraries \
  examples/ra4-arduino-ble-begin-smoke
```

## Paired-board validation

On the dedicated BL616CL/RA4M1 paired-board rig, the runner compiles, uploads
through split SAM-BA, then resets the RA4M1 application and verifies only the
fixed markers through the reference RX path:

```bash
PATH="$HOME/.local/bin:$PATH" \
  python3 tools/run_ra4_arduino_ble_begin_smoke.py
```

To reset and validate the already running Sketch without replacing RA4M1 Flash:

```bash
python3 tools/run_ra4_arduino_ble_begin_smoke.py --skip-upload
```

## Hardware Effect

Uploading this Sketch replaces the current RA4M1 user Sketch, which is
normally `examples/ra4-at-transcript-proxy` during transcript work. It does
not change ESP32-S3 firmware. The proxy source remains available and can be
re-uploaded after this smoke.

`BLE.end()` is called after a successful `BLE.begin()`, but the current
UNO R4 `HCIVirtualTransportATClass::end()` implementation is empty. Therefore
the marker `BLE_END_CALLED` does not prove that `AT+HCIEND` was sent. Do not
use this Sketch as a controller cleanup mechanism.

Expected fixed serial markers are:

```text
RA4_BLE_SMOKE:BOOT
RA4_BLE_SMOKE:BLE_BEGIN=PASS|FAIL
RA4_BLE_SMOKE:BLE_BEGIN_MS=<milliseconds>
RA4_BLE_SMOKE:BLE_END_CALLED
RA4_BLE_SMOKE:DONE
RA4_BLE_SMOKE:RESULT=PASS|FAIL
RA4_BLE_SMOKE:BLE_BEGIN_MS=<milliseconds>
```

`BLE_END_CALLED` appears only after a successful initialization. A `FAIL`
result is evidence for investigation, not a BL616CL pass/fail target by
itself.
