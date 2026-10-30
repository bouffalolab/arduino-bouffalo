# RA4M1 AT UART Transcript Proxy

This temporary RA4M1 Sketch forwards bytes between the user serial port and
the AT serial port:

```text
Host -> BL616CL USB CDC -> RA4M1 Serial -> RA4M1 Serial2 -> BL616CL UART1
BL616CL UART1 -> RA4M1 Serial2 -> RA4M1 Serial -> FTDI reference RX -> Host
```

It is a diagnostic tool, not application firmware. Uploading it replaces the
current RA4M1 Sketch. It does not print boot text and does not implement the
USB CDC 1200/2400 baud control protocol, so keep both UARTs at 115200 baud.

On the final BL616CL carrier, there is no separate reference receiver: one
USB CDC stream reaches `Serial`, and the proxy makes the full request/response
path observable through that same stream. The final-carrier runner uses the
paced BOSSA transport, then restores the WiFiS3 scan Sketch:

```bash
PATH="$HOME/.local/bin:$PATH" python3 tools/run_ra4_at_uart_final_carrier.py
```

The normal restore Sketch completes a Wi-Fi scan, so the runner deliberately
expects `AT+GETSTATUS?` to return `2` (`WL_SCAN_COMPLETED`). Pass
`--expected-status <value>` only when a prior state-changing test intentionally
leaves the bridge in a different known state.

For the current hand-wired paired board, run the complete GPIO6/GPIO7 AT UART
gate from the repository root:

```bash
PATH="$HOME/.local/bin:$PATH" python3 tools/run_ra4_at_uart_gate.py
```

The runner verifies the expected USB identities, compiles this Sketch, uploads
it through the split SAM-BA transport, keeps the relay in the RA4M1 runtime
route, and requires byte-exact responses:

```text
AT\r\n                 -> OK\r\n
AT+FWVERSION?\r\n      -> +FWVERSION: 0.6.0\r\nOK\r\n
AT+GETSTATUS?\r\n      -> +GETSTATUS: 0\r\nOK\r\n
```

To repeat only the response checks after a successful upload:

```bash
PATH="$HOME/.local/bin:$PATH" python3 tools/run_ra4_at_uart_gate.py --skip-upload
```

With the proxy already running, run one credential-free, redacted Wi-Fi scan
through the same GPIO6/GPIO7 path:

```bash
PATH="$HOME/.local/bin:$PATH" python3 tools/run_ra4_at_wifi_scan_gate.py
```

The scan runner validates the full AT response only in memory. It prints and
records only the response byte count, row count, elapsed time, terminal `OK`,
and the immediate `GETSTATUS=2` result; it never prints or saves SSIDs,
BSSIDs, or credentials. Pass `--upload-proxy` only when the current RA4M1
Sketch must be replaced with this proxy first.

To validate the state-changing NULL/STA subset without credentials, run:

```bash
PATH="$HOME/.local/bin:$PATH" python3 tools/run_ra4_at_wifimode_gate.py --upload-proxy
```

The runner replaces the current RA4M1 Sketch with this proxy, requires the
preflight STA mode, checks the exact `1 -> 0 -> 1` responses and `GETSTATUS`
readbacks, and attempts to restore STA mode if an intermediate assertion
fails. Omit `--upload-proxy` only when this proxy is already running.

To run the credential-safe STA association lifecycle, set credentials only in
the shell that starts the runner:

```bash
export BL616CL_STA_SSID='test-network'
export BL616CL_STA_PASSWORD='test-password'
PATH="$HOME/.local/bin:$PATH" python3 tools/run_ra4_sta_association_gate.py --upload-proxy
```

`BL616CL_STA_SSID` is required. `BL616CL_STA_PASSWORD` is optional only for an
open network; when set it must be nonempty. The runner accepts ASCII SSIDs and
passwords within the current bridge limits (32 and 64 bytes), quotes them for
the AT parser in memory, and never prints, saves, commits, or includes their
values in an error message. It verifies STA mode, temporarily disables
`RECONNECT`, waits for `GETSTATUS=3`, sends `DISCONNECT`, waits for
`GETSTATUS=0`, and restores the original reconnect setting during cleanup.

This is a state-changing gate. `--upload-proxy` replaces the current RA4M1
Sketch. Once the proxy preflight succeeds, the runner automatically restores
`ra4-wifis3-scan-smoke` before it reports success or failure, so the normal
RA4M1 evidence Sketch is retained. `--keep-proxy` is only for focused
diagnosis and leaves the RA4M1 proxy installed.

For the separately connected original ESP32-S3 UNO R4 baseline board, use the
private credential environment to capture only redacted STA success and
authentication-failure status behavior:

```bash
set -a
. ~/.config/uno-r4-wifi-bridge/sta-test.env
set +a
PATH="$HOME/.local/bin:$PATH" python3 tools/run_esp32_sta_baseline_gate.py
```

This runner is fixed to the original board identity
The baseline helper recognizes the registered original ESP32-S3 UNO R4 serials
and resolves the port from USB identity rather than assuming a fixed
`/dev/ttyACM*` number.
It never prints or saves credentials, request bytes, or raw AT responses. It
uploads the temporary proxy, records only status codes and timings, restores
the original `RECONNECT` setting, then replaces the proxy with the WiFiS3 scan
smoke Sketch and verifies its fixed markers.

## Controlled AP outage and reconnect

An active `RECONNECT=1` policy can only be tested by removing the associated
AP externally. Sending `AT+DISCONNECT` is not a substitute because it measures
the explicit disconnect command rather than AP-loss recovery.

The two reconnect runners accept an executable outage hook and recovery hook.
The hooks must control a dedicated test AP only. They receive
`UNO_R4_STA_RECONNECT_PHASE=outage` or `recovery`, must return zero only after
the AP has reached the requested state, and should obtain any router/API
credentials from a protected local configuration outside this repository.
Their stdout/stderr is discarded, and STA credential environment variables are
removed before either hook starts.

Run the original ESP32-S3 baseline first:

```bash
set -a
. ~/.config/uno-r4-wifi-bridge/sta-test.env
set +a
PATH="$HOME/.local/bin:$PATH" python3 tools/run_esp32_sta_reconnect_gate.py \
  --outage-hook ~/.config/uno-r4-wifi-bridge/ap-outage \
  --recovery-hook ~/.config/uno-r4-wifi-bridge/ap-recovery
```

Then run the BL616CL paired-board comparison:

```bash
set -a
. ~/.config/uno-r4-wifi-bridge/sta-test.env
set +a
PATH="$HOME/.local/bin:$PATH" python3 tools/run_ra4_sta_reconnect_gate.py \
  --upload-proxy \
  --outage-hook ~/.config/uno-r4-wifi-bridge/ap-outage \
  --recovery-hook ~/.config/uno-r4-wifi-bridge/ap-recovery
```

Each runner requires the sequence `connected -> AP outage -> GETSTATUS=0 ->
AP recovery -> GETSTATUS=3`. It prints only deduplicated numeric status
sequences and timing markers. On success or failure it attempts AP recovery,
disables reconnect for cleanup, disconnects the STA, restores the preceding
`RECONNECT` value, and restores the RA4M1 WiFiS3 scan Sketch unless
`--keep-proxy` was requested.

When the Windows COM ports are available, the same gate can be run without
depending on WSL `/dev` visibility:

```powershell
.\tools\run_ra4_at_uart_gate_windows.ps1
```

The Windows runner expects `COM34` to be the BL616CL bridge CDC
(`VID_2341&PID_1002`) and `COM35` to be the FTDI reference receiver
(`VID_0403&PID_6010`). Use `-SkipUpload` to test the proxy already running on
RA4M1. This replaces the current RA4M1 Sketch when upload is enabled.

The equivalent Windows Wi-Fi scan gate is:

```powershell
.\tools\run_ra4_at_wifi_scan_gate_windows.ps1
```

Pass `-UploadProxy` only when it should first replace the current RA4M1 Sketch.
