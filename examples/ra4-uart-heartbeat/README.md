# RA4M1 User-UART Heartbeat

This temporary Sketch validates the hand-wired paired-board user-UART path:

```text
Host -> BL616CL USB CDC -> GPIO34 -> RA4M1 Serial RX
RA4M1 Serial TX -> FTDI reference receiver -> Host
```

It emits `RA4_HEARTBEAT:BOOT` once, then `RA4_HEARTBEAT:<count>` every
second. A newline-terminated input line is returned as `RA4_ECHO:<line>`.

Compile it with:

```bash
arduino-cli compile --fqbn arduino:renesas_uno:unor4wifi examples/ra4-uart-heartbeat
```

Upload it through the verified split UART runner:

```bash
python3 tools/run_ra4_bossac_split.py
```

This replaces the current RA4M1 Sketch. The runner first sets BL616CL CDC to
1200 baud to enter RA4M1 SAM-BA, uses the FTDI RTS# relay in its released
runtime position, then runs Arduino `bossac -e -w -R`. It verifies the boot
marker, two heartbeat lines, and a nonce echo before reporting success.

For manual runtime interaction after upload:

```bash
python3 tools/monitor_ra4_split_uart.py
```

The monitor sends keyboard input through `/dev/ttyACM1`, shows RA4M1 output
from `/dev/ttyUSB0`, keeps the relay in the RA4M1 runtime position, and exits
with `Ctrl-]`.
