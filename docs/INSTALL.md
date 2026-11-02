# Install and run

Prerequisites: Linux x86_64, a BL616CL 1V0 UNO R4 WiFi board, USB access, and an
approved release in `bouffalolab/arduino-bouffalo`. Windows/macOS and 1V1 are untested.
Do not use the older legacy `v0.1` tag as this new platform's package index.

## Arduino IDE

1. Open Preferences and add the URL below to **Additional boards manager URLs**.
2. Open Boards Manager, search for **Bouffalo Lab BL boards**, and install **0.1.0**.
3. Select **UNO R4 WiFi (BL616CL coprocessor)** when programming BL616CL.
4. Select the matching USB port, compile Blink, and upload. Open Serial Monitor
   for examples that print output. See [recovery](RECOVERY.md) if the bridge is
   already running or the board is in ROM download mode.

```text
https://raw.githubusercontent.com/bouffalolab/arduino-bouffalo/master/package_bouffalolab_index.json
```

The URL will return 404 until the first approved index is published. A loopback
URL such as `127.0.0.1:8977` works only while its local test server is running;
it is not a public installation channel. Toolchain extraction can take longer
than the other installation steps. Wait for the installation result.

## Arduino CLI

The reviewed CLI version is 1.5.1. With the public index available:

```sh
arduino-cli core update-index --additional-urls https://raw.githubusercontent.com/bouffalolab/arduino-bouffalo/master/package_bouffalolab_index.json
arduino-cli core install bouffalolab:bl@0.1.0 --additional-urls https://raw.githubusercontent.com/bouffalolab/arduino-bouffalo/master/package_bouffalolab_index.json
arduino-cli board list
arduino-cli compile -b bouffalolab:bl:unor4wifi_bl616cl --build-path /path/to/build /path/to/Blink
arduino-cli upload -b bouffalolab:bl:unor4wifi_bl616cl -p /dev/serial/by-path/YOUR_DEVICE --input-dir /path/to/build /path/to/Blink
```

Replace example paths and select the device from its actual USB identity/location.
The index installs `bl616cl-libs` 0.9.0, `bl-flash-tools` 0.2.0 and Xuantie 2.6.1.
No SDK checkout or Python installation is required by the installed platform.

## Use the UNO bridge and RA4M1 examples

Build the bridge repository's `UNOR4USBBridgeBL` sketch for the BL616CL board.
Its default mapping is 1V0. Keep the default partition scheme and debug logging off.
Once the bridge is installed, select **Arduino UNO R4 WiFi** from the official
Renesas 1.6.0 package for RA sketches. WiFiS3 is supplied by that package.
PhoneMatrix additionally uses ArduinoBLE 2.1.0. See the bridge example and the
explicit BlueZ compatibility patch; the patch is not applied automatically.

Create your own `arduino_secrets.h` when a sketch needs Wi-Fi credentials. Never
place a real password or a private key in a shared sketch or release package.
Use the release's exact tested PhoneMatrix binary when reproducing its phone
acceptance; a later GUI-uploaded image is a separate artifact.
