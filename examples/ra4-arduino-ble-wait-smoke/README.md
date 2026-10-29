# RA4M1 ArduinoBLE Wait Smoke

This temporary RA4M1 Sketch performs one `BLE.begin()` followed by one
`BLE.poll(250)` call through the production UNO R4 WiFi `ArduinoBLE`
transport. Unlike `BLE.scan()` and `BLE.available()`, the explicit nonzero
timeout reaches `HCI.poll(timeout)`, which calls
`HCIVirtualTransportATClass::wait(timeout)`. On UNO R4 WiFi that emits
`AT+HCIWAIT=<timeout>\n\r` on the RA4M1-to-ESP32-S3 AT UART.

It does not scan, advertise, connect, pair, store credentials, read or print
BLE addresses, or print BLE advertisement/HCI payload data. The serial output
contains only fixed markers and elapsed times. The cached result repeats every
two seconds, without issuing another BLE command.

`RA4_BLE_WAIT_SMOKE:RESULT=PASS` means `BLE.begin()` succeeded, the single
bounded `BLE.poll(250)` call returned, and its elapsed time was at most the
fixed 2000 ms compatibility bound. The extra margin permits command framing
and a possible controller event while still detecting a timeout that is
materially longer than the caller requested. The observed elapsed time is
always printed; it can be shorter than 250 ms when an event is already
available.

This is a baseline-observation gate, not a requirement that BL616CL copy a
known bug. An out-of-bound result is evidence to trace through the ESP32-S3
bridge and RA4M1 transport before defining the BL616CL `+HCIWAIT` contract.
It does not validate scanning, connection, GATT, BLE throughput, cleanup, or
BL616CL compatibility.

On the ESP32-S3 `0.6.0` baseline tested on 2026-08-17, this Sketch reported
`BLE_BEGIN_MS=195` and `WAIT_ELAPSED_MS=20003`, so it correctly reported
`RESULT=FAIL`. That observation is intentionally retained as a compatibility
risk rather than normalized into a BL616CL target.

## Build

```bash
arduino-cli compile \
  --fqbn arduino:renesas_uno:unor4wifi \
  --libraries libraries \
  examples/ra4-arduino-ble-wait-smoke
```

## Hardware Effect

Uploading this Sketch replaces the current RA4M1 user Sketch. It does not
change the ESP32-S3 firmware or the independent BL616CL development board.

`BLE.end()` is called after the wait only when initialization succeeded, but
the current UNO R4 `HCIVirtualTransportATClass::end()` implementation is
empty. Therefore `BLE_END_CALLED` does not prove that `AT+HCIEND` was sent.
