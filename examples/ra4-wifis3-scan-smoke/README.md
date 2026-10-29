# RA4M1 WiFiS3 scan smoke

This Sketch validates the Arduino user-facing `WiFiS3` path through the
BL616CL bridge:

```text
WiFi.scanNetworks()
  -> RA4M1 AT client
  -> BL616CL GPIO6/GPIO7
  -> WifiService
  -> Bouffalo Wi-Fi scan
```

It deliberately prints only firmware version, scan count, elapsed time, and a
pass/fail marker. It never prints SSIDs, BSSIDs, RSSI values, credentials, or
raw scan rows.

The paired-board runner compiles and uploads this Sketch through the split
SAM-BA path. Uploading replaces the current RA4M1 Sketch; it does not write
BL616CL Flash:

```bash
PATH="$HOME/.local/bin:$PATH" python3 tools/run_ra4_wifis3_scan_smoke.py
```

To validate the Sketch already running on RA4M1 without replacing its Flash:

```bash
PATH="$HOME/.local/bin:$PATH" python3 tools/run_ra4_wifis3_scan_smoke.py --skip-upload
```
