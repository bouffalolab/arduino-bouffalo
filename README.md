# Arduino BL616CL platform — UNO R4 bridge status

BLE UART HCI（AT+HCI）的状态、复现步骤与历史结论见
[docs/BLE-UARTHCI.md](docs/BLE-UARTHCI.md)。

This local development platform implements FQBN
`bouffalo:bl616cl:unor4_bl616cl` for BL616CL DK bring-up and the UNO R4 bridge.
The checked-in bridge build artifact is produced from the same FQBN. Linux
post-processing tools are supplied by the generator and the checked-in macOS
post-processor is used automatically on Apple Silicon hosts.

For normal Arduino compilation the platform is self-contained (a git clone
plus the gitignored host-tool directories below).  Its layout is:

    hardware/bouffalo/bl616cl/
    ├── cores/bl616cl/                  Arduino Core sources
    ├── libraries/esp32-compat/         ESP32 Arduino API compatibility stubs
    ├── variants/bl616cldk/             pin mapping (pins_arduino.h)
    ├── tools/sdk/bouffalo_sdk/         Bouffalo SDK git submodule (pinned)
    │                                    (runtime headers/archives/boot2/DTS
    │                                     are built from it at compile time)
    ├── tools/partitions/               partition TOML (compile-time → bin)
    ├── tools/runtime_bundle/           compile-time runtime builder, patches
    └── tools/{Xuantie-900-gcc,bflb_fw_post_proc,bouffalo_flash_cube}
        (generated, in .gitignore)

The `recipe.hooks.prebuild` hook in `platform.txt` materialises the per-build
SDK runtime from the submodule before every sketch compile; see
`tools/runtime_bundle/README.md`.

Implemented:

- standard Arduino `.ino` preprocessing and `setup()`/`loop()` runtime;
- C/C++ global constructors;
- BL616CL board startup and FreeRTOS scheduler;
- GPIO and `LED_BUILTIN` stage-1 mapping;
- `millis()`, `micros()`, `delay()`, and `delayMicroseconds()`;
- console `Serial` on BL616CL DK UART0 (GPIO34 TX / GPIO35 RX, 2 Mbit/s);
- `Serial1` on GPIO6 TX / GPIO7 RX (RA4M1 AT channel); `HardwareSerial` uses
  interrupt-driven reception with a 4096-byte ring buffer, receive/overflow
  counters, RX clear, and in-place baud-rate changes (PR #10 UART port;
  2 Mbaud UART0 and bench UART1 on GPIO27/28 verified on hardware);
- CherryUSB device support for a CDC ACM + HID composite endpoint, backed by
  the Arduino-style `USBCDC`/`USBHID` compatibility classes;
- C++17 with exceptions and RTTI disabled;
- `.elf`, `.map`, post-processed `.bin`, boot2, partition, and eFuse side cars;
- modern BL616CL `bflb_fw_post_proc` and `BLFlashCommand` integration.

The `bl616cldk` variant now carries the UNO R4 carrier pin map: RA4M1 AT on
UART1 GPIO6/7, RA4M1 log/flash on UART0 GPIO34/35, and the SWD/MD/RESET
control lines in the bridge firmware (GPIO8/9/3/10). GPIO32/33 remain
reserved for USB. The stage-1 4 MiB partition limits the primary
firmware slot to 2 MiB; eFuse files are exported for traceability but are not
burned by the normal Arduino upload action.

## First-time setup

Clone this repo into `hardware/bouffalo/bl616cl/` inside an Arduino CLI
workspace and check out the SDK submodule:

    git submodule update --init tools/sdk/bouffalo_sdk

The repo excludes host tools (toolchain, `bflb_fw_post_proc`,
`BLFlashCommand`) via `.gitignore`.  Install them once with the legacy
generator — it writes a `tools/sdk/<chip>` bundle that the current flow no
longer consumes (every compile rebuilds the runtime from the submodule via
`build_sdk_runtime.py`); only the host tools it installs are needed.
`--tools-only` refreshes them from an existing `tools/sdk/<chip>` directory:

    python3 hardware/bouffalo/bl616cl/tools/runtime_bundle/generate_runtime_bundle.py \
      --sdk /path/to/bouffalo_sdk \
      --chip bl616cl \
      --toolchain /path/to/Xuantie-900-gcc

Make sure `arduino-cli` is in PATH or set `ARDUINO_CLI=/path/to/arduino-cli`.

## Runtime maintenance

The runtime is rebuilt automatically whenever its inputs change (SDK commit
or dirty state, patch set, `defconfig`/`FreeRTOSConfig.h`, toolchain version,
extra controller archives).  Cached entries live under
`~/.cache/arduino-bouffalo/sdk-runtime/` (override with
`BOUFFALO_SDK_CACHE`).  Pin a new SDK release by checking out the tag in the
submodule and staging the submodule pointer:

    git -C hardware/bouffalo/bl616cl/tools/sdk/bouffalo_sdk fetch --tags
    git -C hardware/bouffalo/bl616cl/tools/sdk/bouffalo_sdk checkout v2.3.36
    git add hardware/bouffalo/bl616cl/tools/sdk/bouffalo_sdk

Force a rebuild or keep the SDK build tree for inspection:

    python3 hardware/bouffalo/bl616cl/tools/runtime_bundle/build_sdk_runtime.py \
      --platform-root hardware/bouffalo/bl616cl \
      --chip bl616cl --board bl616cldk --out /tmp/rt/bl616cl --force --keep-work

See `tools/runtime_bundle/README.md` for multi-chip support and details.

## Verify M0

    arduino-cli board details \
      --config-file arduino-cli.yaml \
      --fqbn=bouffalo:bl616cl:unor4_bl616cl

## Compile M1 smoke sketches

    arduino-cli compile --config-file arduino-cli.yaml \
      --fqbn=bouffalo:bl616cl:unor4_bl616cl \
      hardware/bouffalo/bl616cl/examples/Blink

    arduino-cli compile --config-file arduino-cli.yaml \
      --fqbn=bouffalo:bl616cl:unor4_bl616cl \
      hardware/bouffalo/bl616cl/examples/Serial

## Compile the UNO R4 bridge

`libraries/esp32-compat` provides the ESP32-style API used by the upstream
`uno-r4-wifi-usb-bridge` sketch, so the sketch can be built without modifying
its command handlers:

    arduino-cli compile --config-file arduino-cli.yaml \
      --fqbn=bouffalo:bl616cl:unor4_bl616cl \
      /path/to/uno-r4-wifi-usb-bridge/UNOR4USBBridge

The current support boundary is:

| Bridge dependency | Current status | Evidence / limitation |
| --- | --- | --- |
| USB CDC ACM + CMSIS-DAP HID | Implemented and hardware-tested | CDC echo and DAP command round trips pass on BL616CL DK. SWD signal timing and final carrier wiring are still open. |
| WiFi STA, scan, DHCP/static IPv4, DNS, ping | Implemented and hardware-tested | `wifi_mgmr`/wl80211 backend; bridge AT smoke tests pass. SoftAP/APSTA, IPv6, auto-connect and persistent WiFi settings are not implemented. |
| TCP client/server and UDP | Implemented and hardware-tested | lwIP socket backend; AT TCP/UDP echo tests pass. |
| TLS client | Implemented and hardware-tested | mbedTLS 2.28 backend (SDK `CONFIG_MBEDTLS_V2`); CA loading and HTTPS GET pass. |
| SPIFFS/FS and Preferences | Implemented in the compatibility layer | LittleFS on the `media` partition and EasyFlash on PSM pass `StorageTest`; the bridge image still has a partition-table/app overlap that must be fixed before relying on storage in a deployed bridge image. |
| BLE AT/HCI transport | Implemented and hardware-tested (2026-10-03) | Links the SDK-shipped `libbtblecontroller_bl616cl_uarthci.a` (1.6.210); `AT+HCIBEGIN/HCIWRITE/HCIREAD` and the Reset/LE advertising sequence were verified on air. Other HCI/ACL/connection commands and long-term stability remain untested. |
| RA4M1 OTA download/update | Not implemented | `Update.h`, `Arduino_ESP32_OTA.h`, and `BossaArduino.h` contain fail-safe stubs; `BossaUnoR4WiFi::program()` therefore cannot flash the RA4M1. |

PR [#10](https://github.com/bouffalolab/arduino-bouffalo/pull/10) was inspected
at `e6c3179b24bdf1954bb74e3676d0244404bf585a` on 2026-09-29. It adds a
separate bridge runtime profile, provisional carrier mapping and UART/runtime
changes, but contains no SWD timing implementation or calibration results;
its `TODO.md` still lists SWD timing calibration as incomplete. The local
bridge `dap_config.h` still uses the ESP32 SWD pins (SWCLK 7 / SWDIO 8) and
delay constants (7700 / 2400000 Hz), rather than the carrier allocation
(SWCLK 12 / SWDIO 13). This PR alone does not establish BL616CL SWD support.

The existing bridge ELF/bin artifacts under
`uno-r4-wifi-usb-bridge/UNOR4USBBridge/build/bouffalo.bl616cl.unor4_bl616cl/`
confirm that the complete command surface still compiles and links. A green
build does not imply that every AT command is functional: unsupported paths
return their compatibility-layer failure values.
