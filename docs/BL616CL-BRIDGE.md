# BL616CL UNO R4 bridge

`UNOR4BL616CLBridge/` is the independent BL616CL bridge application. Select
`bouffalo:bl616cl:unor4_bl616cl_provisional`; the Ai-M64L board remains a separate
platform target. The provisional board definition describes the paired carrier,
not a production PCB or a DK pin map.

## Build

Install Arduino CLI, the Arduino Renesas UNO platform and the Xuantie toolchain
expected by this platform. Put `arduino-cli` on PATH or set `ARDUINO_CLI`.
Initialize the SDK using `python3 tools/prepare_bouffalo_sdk_source.py --init`,
then run `./compile-bl616cl-bridge.sh`. Outputs are in `build/bridge-image/`.

The bridge uses its own pinned SDK and `tools/bridge_runtime/` source profile;
it does not use the Ai-M64L wireless ABI. SDK repositories currently require
access authorization. Configure the SSH user locally in SSH configuration;
repository URLs do not contain a developer account. Do not substitute an
unverified public SDK revision. RF/PHY/controller supplier binaries and ROM ABI
boundaries are declared in the source and proprietary manifests. License notices
are in `licenses/bl616cl/`.

## Interfaces and storage

The bridge implements WiFiS3 AT framing, CDC serial, 64-byte CMSIS-DAP HID,
Wi-Fi/TCP/UDP/TLS, BLE raw HCI, files/Preferences/SoftSE, RA4 OTA and BL self-FOTA.
The USB contract is VID/PID `2341:1002`. Board wiring is defined by the selected
variant and application configuration.

The frozen 4MiB layout is `partition_cfg_4M_bridge_v1.toml`. FW A starts at
`0x10000`; FW B starts at `0x210000` and shares its region with `mfg`. FW B is
1440KiB, limiting image size for updates. User files, Preferences/keys and the
independent RA4 OTA cache are separate from firmware slots. Preserve factory
and persistent-data regions when updating.

RA4 OTA decodes and validates at most 240KiB before SAM-BA programming. Reset
revokes cached programming authorization; stale bytes do not authorize a flash.
RA4's single application region does not provide atomic A/B rollback.

BL self-FOTA takes a postprocessed BFNP application followed by a 32-byte SHA256.
Use `BLFOTABEGIN`, raw `BLFOTADATA` blocks (maximum 16384B), then `BLFOTAFINISH`;
`BLFOTAABORT` cancels a session. Only a fully verified inactive image is activated.
This does not update boot2, partitions or the RA4 application. SHA256 checks
transport integrity; application health rollback and signed-image authenticity
are not provided by this protocol.

## Update packages

`tools/bl616cl_release.py pack --build <build-dir> --output <new-dir> --version
<version>` creates a local development package. Run its `verify` command before
use. Packages contain three firmware images, updater/verifier, this guide,
source/proprietary manifests and license notices. Debug ELF/map/preprocessed
sources, local runtime manifests, logs and Flash backups are excluded.

Use `update_bl616cl_release.py --help` for explicit target binding and Flash tool
arguments. Preserve a private recovery backup before changing partitions or a
complete device image. Do not distribute that backup.

## Development tests

Host regression entry points are `test-bl616cl-at-transport.sh` and
`test-arduinoble-host.sh`. Hardware runners require a selected test board and
may replace its RA4 application. Configure `UNO_R4_BASELINE_SERIAL` (or the
comma-separated `UNO_R4_BASELINE_SERIALS`) for the ESP comparison board;
there is no default personal-device allowlist. Windows Flash runners take
`-FlashTool` or `BL616CL_FLASH_TOOL`. `UNO_R4_REFERENCE_ROOT` selects external
ESP test-fixture source when needed. Test credentials must stay in a private
configuration file outside the repository; no credentials or device backups
belong in Git. Build outputs and local evidence belong under ignored `build/`.
