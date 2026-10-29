# BL616CL AT Golden Transcripts

These host regressions preserve only observable AT behavior that can currently
be confirmed without a BL616CL carrier board.

## Provenance

- `source-static`: response is directly determined by a checked-in
  `UNOR4USBBridge/` command handler and can be asserted now.
- `hardware-capture-required`: response depends on Wi-Fi, sockets, BLE,
  filesystem contents, timing, or actual ESP32-S3 runtime state. Do not invent
  a BL616CL expected response; capture an ESP32-S3 transcript first.

The executable vectors currently live in
`tests/bl616cl_at_transport_test.cpp` so binary output, including
`+FWVERSION_U32?`, remains byte-accurate.

## Dynamic ESP32-S3 Capture

Dynamic Wi-Fi, socket, BLE, filesystem, and timing behavior must be captured
from an ESP32-S3 baseline bridge before it is adopted as a BL616CL golden
vector. The checked-in non-secret Wi-Fi capture plan is
`tests/at_golden/cases/dynamic_wifi.json`.
It contains read-only, invalid-input, and source-proven no-op cases. The separate
`tests/at_golden/cases/stateful_wifi.json` records all planned Wi-Fi mutations.

The normal UNO R4 WiFi USB CDC port does not reach the ESP32-S3 AT UART. To
capture it, first compile and, only with explicit approval, upload
`examples/ra4-at-transcript-proxy/`. That temporary RA4M1 Sketch is a
byte-transparent bridge between PC USB CDC `Serial` and ESP32-S3 AT `Serial2`
at 115200 baud. Uploading it replaces the existing RA4M1 user Sketch. It has
no serial logging and intentionally does not implement the normal bridge's
1200/2400 baud control behavior.

Never place SSIDs, passwords, tokens, certificates, or private socket payloads
in a checked-in case file or transcript. Capture private scenarios in a
separate protected location and redact their raw request/response bytes before
using them as an interoperability reference.

Validate the checked-in plan without hardware:

```bash
python3 tools/capture_esp32_at_transcript.py --validate-cases
python3 tools/capture_esp32_at_transcript.py --list-cases
python3 tools/capture_esp32_at_transcript.py --case wifi_scan --dry-run
```

After the proxy has been deliberately uploaded and the normal USB serial
device has been identified, capture one case using an explicit device path:

```bash
python3 tools/capture_esp32_at_transcript.py \
  --case wifi_scan \
  --port /dev/ttyACM0 \
  --firmware-version 0.6.0 \
  --output /tmp/esp32-at-wifi-scan.json
```

The tool never auto-detects a serial port. It stores the request and response
as hex, base64, and printable ASCII together with timeout and elapsed-time
evidence.

## Stateful Wi-Fi Capture

State-changing cases are deliberately separated from the default capture plan.
They contain no credentials and cannot be executed in one step. First create a
read-only preflight record for the exact case:

```bash
python3 tools/capture_esp32_at_transcript.py \
  --case-file tests/at_golden/cases/stateful_wifi.json \
  --case reconnect_write_disable \
  --preflight \
  --port /dev/ttyACM0 \
  --firmware-version 0.6.0 \
  --output /tmp/esp32-at-reconnect-preflight.json
```

Review the recorded `GETSTATUS`, `WIFIMODE`, `RECONNECT`, and `AUTOCONNECT`
responses, confirm that the case preconditions are true, and decide the manual
recovery command from the observed values. Only after separate explicit
authorization may the mutation be captured:

```bash
python3 tools/capture_esp32_at_transcript.py \
  --case-file tests/at_golden/cases/stateful_wifi.json \
  --case reconnect_write_disable \
  --allow-state-change \
  --preflight-record /tmp/esp32-at-reconnect-preflight.json \
  --port /dev/ttyACM0 \
  --firmware-version 0.6.0 \
  --output /tmp/esp32-at-reconnect-write.json
```

The tool verifies that the preflight was produced for the same case and that
all pre-state requests completed. It does not execute recovery commands:
network settings and association state are not safe to infer from a checked-in
plan. `wifi_disconnect_run` may require a private, separately authorized
credential procedure to reconnect.

Review a captured response against the ESP32-S3 command handler, normalize
only explicitly documented volatile fields, then add a focused BL616CL host
regression vector with the capture file and test conditions recorded in
`../../docs/项目进展.md`. A successful capture is baseline evidence, not BL616CL hardware
validation.

## BLE Raw HCI Capture

`tests/at_golden/cases/dynamic_ble_hci.json` is a separate temporary-state
plan for the ESP32-S3 raw HCI adapter. It records one controller H4 Reset:

```text
HCIBEGIN
HCIWRITE=4 + 01 03 0C 00
HCIWAIT=1000 with LFCR line ending
HCIAVAILABLE?
HCIREAD
HCIEND
```

`HCIWRITE` is intentionally modeled as an ASCII header and raw bytes in one
capture step. This is the same two-stage wire contract used by
`HCIVirtualTransportAT::write()`: no separator may be inserted after the
header's CRLF. `HCIWAIT` uses the `\n\r` order emitted by the current
RA4M1 ArduinoBLE adapter source; the capture records the exact request bytes
instead of silently normalizing them to CRLF.

The case is marked `temporary`, so first collect and review its read-only
firmware-version preflight. The case requires the exact ESP32-S3 `0.6.0`
response; a timeout or a different response is written to the local record but
fails the command and cannot authorize the later state-changing sequence:

```bash
python3 tools/capture_esp32_at_transcript.py \
  --case-file tests/at_golden/cases/dynamic_ble_hci.json \
  --case hci_reset_round_trip \
  --preflight \
  --port /dev/ttyACM0 \
  --firmware-version 0.6.0 \
  --output /tmp/esp32-at-ble-hci-preflight.json
```

Then capture the sequence only after the preflight confirms the expected
baseline:

```bash
python3 tools/capture_esp32_at_transcript.py \
  --case-file tests/at_golden/cases/dynamic_ble_hci.json \
  --case hci_reset_round_trip \
  --allow-state-change \
  --preflight-record /tmp/esp32-at-ble-hci-preflight.json \
  --port /dev/ttyACM0 \
  --firmware-version 0.6.0 \
  --output /tmp/esp32-at-ble-hci-reset.json
```

The final planned step is `HCIEND`; it normally returns the controller to the
pre-capture state. The capture tool never invents or automatically sends a
recovery command after an interrupted sequence. If `HCIEND` did not complete,
manually send `AT+HCIEND\r\n` through the proxy before attempting another BLE
test. Review the resulting raw H4 event and all response framing before adding
a checked-in compatibility contract.

The direct plan intentionally exercises `HCIWAIT` before `HCIAVAILABLE?`.
That does not reproduce the first Reset issued by `BLE.begin()`: the current
ArduinoBLE `HCIClass::sendCommand()` polls with timeout zero, so it calls
`HCIAVAILABLE?` directly rather than issuing `HCIWAIT`. Treat any difference
between this direct sequence and a RA4M1 ArduinoBLE smoke as a test-condition
distinction that requires its own evidence, not as a contradiction.

After the RA4M1 `BLE.poll(250)` smoke observed a 20-second return time, this
file also defines two separate cases. First,
`hci_end_after_ra4_wait_smoke` stops the controller that the RA4M1 transport
may have left active because its `end()` is empty. Only after that case reaches
a terminal response should `hci_wait_250_round_trip` start a fresh controller
and measure the LFCR `HCIWAIT=250` exchange separately from the following
`HCIAVAILABLE?`. The round trip sends no HCI packet or BLE application data.
Its 15-second per-step capture timeout is diagnostic only; it must not be
copied into a BL616CL compatibility target.

On the ESP32-S3 `0.6.0` baseline captured on 2026-08-17, the direct
round-trip completed with `HCIBEGIN` in 166.924 ms, `HCIWAIT=250` in
411.930 ms, `HCIAVAILABLE?` returning `ERROR` in 159.835 ms, and `HCIEND` in
165.361 ms. A separate RA4M1 timing smoke measured the production
`HCITransport.wait(250)` and following `HCITransport.available()` at about
10 seconds each. The direct proxy result therefore does not represent the
RA4M1 Modem parser timing; neither the 20-second baseline behavior nor the
diagnostic timeout belongs in the BL616CL compatibility contract.

The temporary proxy must not use `Stream::availableForWrite()` as a forwarding
gate. The UNO R4 WiFi RA4M1 core's hardware `UART` class inherits the default
`Print::availableForWrite()` implementation, which returns zero. The proxy
instead writes one byte to the destination before consuming it from the source;
the RA4M1 UART `write()` call blocks until the byte is accepted.

## Wi-Fi Contract Fixture

`tests/at_golden/contracts/esp32_s3_wifi_0.6.0.json` is the redacted
compatibility contract derived from the 2026-08-13 ESP32-S3 `0.6.0` capture.
It distinguishes exact error frames from dynamic values:

- `+GETSTATUS?`, `+WIFIMODE?`, `+RECONNECT?`, and `+AUTOCONNECT?` constrain
  response framing and permitted values without freezing the captured runtime
  values.
- The 2026-08-14 `AT+RECONNECT=0` and `AT+RECONNECT=1` baseline writes each
  return exactly `OK\r\n`; their state-changing effects are recorded separately
  from this framing contract.
- `AUTOCONNECT` is not a dynamic setting on this ESP32-S3 0.6.0 baseline.
  `WiFiSTAClass::setAutoConnect()` and `getAutoConnect()` are deprecated,
  unconditional `false` stubs. The AT handler ignores the setter return, then
  the AT framework appends `OK\r\n`; a read always returns
  `+AUTOCONNECT: 0\r\nOK\r\n`. The no-op writes belong in the default
  non-mutating capture plan, not the stateful plan.
- The captured `AT+WIFIMODE=0` and `AT+WIFIMODE=1` writes each return
  `+WIFIMODE: \r\nOK\r\n`: unlike `WIFIMODE?`, the write form has only one
  CRLF before `OK`. In the credential-free round trip, `1 -> 0` reads back
  mode `0` and status `255`; restoring `0 -> 1` reads back mode `1` and
  idle status `0`.
- `+WIFIMODE?` requires two CRLF sequences before `OK`.
- `+WIFISCAN` permits `OK` alone for an empty scan, or
  `+WIFISCAN: <decimal-row-byte-count>|` followed by one or more rows. The
  decimal count is required by `WiFiS3`'s sized-response parser. The checked-in
  example uses synthetic SSIDs and BSSIDs.
  The capture plan immediately follows a scan with `AT+GETSTATUS?`: on
  2026-08-14, after the credential-free `WIFIMODE 1 -> 0 -> 1` restoration,
  an initial idle `0` became scan-complete `2`. This is recorded as an observed
  state transition, not a constant required for every radio environment.

Validate the contract without hardware:

```bash
./test-at-golden-contract.sh
```

The contract is an acceptance target for the future BL616CL Wi-Fi AT service;
it does not claim that Wi-Fi functionality is implemented today.

### AUTOCONNECT Semantic Matrix

| AT request | ESP32-S3 0.6.0 path | Handler/framework response | Observable state/readback | BL616CL baseline |
| --- | --- | --- | --- | --- |
| `AT+AUTOCONNECT=1` | `atoi("1")` -> `true`; deprecated setter returns `false` without storing it | Handler ignores return; framework writes `OK\r\n` | No state change; `AT+AUTOCONNECT?` returns `0` | Accept and return `OK`; retain readback `0` |
| `AT+AUTOCONNECT?` | Deprecated getter returns `false` unconditionally | `+AUTOCONNECT: 0\r\nOK\r\n` | Always `0` for this Core revision | Return `0` |
| `AT+WIFIMODE=<mode>,<autoConnect>` | Optional second argument calls the same deprecated setter | Mode command's own response framing applies | Optional argument has no AUTOCONNECT effect | Do not bind this optional argument to a native auto-connect setting |

The distinction from `RECONNECT` is deliberate: `setAutoReconnect()` stores
the supplied boolean and `getAutoReconnect()` returns that stored value, while
the two `AutoConnect` APIs do neither. Any future decision to make
`AUTOCONNECT` functional on BL616CL must be a separately approved compatibility
change with RA4M1 regression evidence.

## Wi-Fi Status Tracking

`UNOR4BL616CLBridge/wifi_status.*` records the source-static ESP32-S3 event
mapping that drives `AT+GETSTATUS?`. It preserves the observed mappings from
`UNOR4USBBridge/UNOR4USBBridge.ino`: ready/STA start/STA disconnect/AP stop
produce idle `0`; scan completion produces `2`; STA connected produces `3`;
STA stop produces `255`; AP start produces `7`; and AP station connect produces
`8`. AP station disconnect and unrelated events preserve the previous value.
The SoftAP command failure path may explicitly set `9`.

The tracker is platform-neutral and is covered by
`./test-bl616cl-at-transport.sh`. It is not yet registered with Bouffalo SDK
`EV_WIFI`: future `WifiService` must translate the SDK's init, scan, connected,
disconnect, AP start/stop, and AP station add/delete codes into the tracker.
Do not assign ESP32-S3 values `1`, `4`, `5`, or `6` from a Bouffalo failure
event until the source policy and hardware transcript for that condition are
recorded.

## Current Source-Static Vectors

| Input | Expected response | Source |
| --- | --- | --- |
| `AT` | `OK` | AT transport contract |
| `AT+GMR` | `<len>` then `OK` | `cmds_esp_generic.h` |
| `AT+CMD?` | `+CMD: <test>` then `OK` | `cmds_esp_generic.h` |
| `AT+FWVERSION?` | `+FWVERSION: 0.6.0` then `OK` | `cmds_esp_generic.h` |
| `AT+FWVERSION_U32?` | four bytes `00 06 00 00` then `OK` | `cmds_esp_generic.h` |
| `AT+GETSTATUS?` while idle | `+GETSTATUS: 0` then `OK` | `cmds_wifi_station.h` |
| Wrong mode, malformed, or unknown command | `ERROR` | command dispatch contract |

`AT+RESET` is deliberately excluded: the ESP32-S3 implementation restarts,
while BL616CL reset/recovery semantics require a separate approved design and
hardware validation.

## Backpressure Contract

The BL616CL transport builds one complete AT response in a fixed-capacity
buffer before it is exposed to the UART backend. The UART may accept only part
of that response on a poll iteration; the remaining bytes stay queued in
order. While an output response remains queued, parsing pauses before the
next command. This prevents silent truncation of `OK`, `ERROR`, binary
payloads, or response framing.
