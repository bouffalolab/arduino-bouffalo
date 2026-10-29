# RA4M1 ArduinoBLE Transport Timing Smoke

This temporary RA4M1 Sketch separates the two calls made by
`HCIClass::poll(250)` after a successful `BLE.begin()`:

```text
HCITransport.wait(250)
HCITransport.available()
```

It prints fixed elapsed-time markers for each call and the integer returned by
`available()`. It does not scan, advertise, connect, pair, exchange application
data, read BLE identities, or print HCI payload bytes.

The Sketch includes the production ArduinoBLE `HCITransport` interface. It
therefore measures the same UNO R4 `HCIVirtualTransportAT` implementation that
`BLE.poll(250)` uses, while making each transport operation observable
separately.

`BLE.end()` is called after the measurements, but the current UNO R4
`HCIVirtualTransportATClass::end()` implementation is empty. It does not send
`AT+HCIEND`; after this Sketch is used, upload the temporary AT proxy and
perform the explicit `HCIEND` cleanup capture before another BLE test.

## Build

```bash
arduino-cli compile \
  --fqbn arduino:renesas_uno:unor4wifi \
  --libraries libraries \
  examples/ra4-arduino-ble-transport-timing-smoke
```

## Hardware Effect

Uploading this Sketch replaces the current RA4M1 user Sketch. It does not
modify the ESP32-S3 baseline firmware or the independent BL616CL development
board.
