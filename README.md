# Arduino BL616CL platform — stage 1

This local development platform implements FQBN
`bouffalo:bl616cl:unor4_bl616cl` for the stage-1 M0/M1 milestone. Linux
post-processing tools are supplied by the generator and the checked-in macOS
post-processor is used automatically on Apple Silicon hosts.

The platform is self-contained for normal Arduino compilation: it does not
reach into `arduino-bouffalo` or a Bouffalo SDK checkout. Its layout is:

    hardware/bouffalo/bl616cl/
    ├── cores/bl616cl/                  Arduino Core sources
    ├── libraries/esp32-compat/         ESP32 Arduino API compatibility stubs
    ├── variants/bl616cldk/             pin mapping (pins_arduino.h)
    ├── tools/sdk/bl616cl/              chip-level runtime bundle
    │   ├── lib/                         SDK archives
    │   ├── lib_board/                   board BSP (libapp.a)
    │   ├── include/                     SDK headers + autoconf.h
    │   ├── include/board/               board headers
    │   ├── boot2/                       boot2 binary
    │   ├── dts/                         DTS config
    │   └── ld                           linker script
    ├── tools/partitions/               partition TOML (compile-time → bin)
    ├── tools/runtime_bundle/           multi-chip bundle generator
    └── tools/{Xuantie-900-gcc,bflb_fw_post_proc,bouffalo_flash_cube}
        (generated, in .gitignore)

The SDK checkout is needed only when deliberately regenerating the checked-in
runtime bundle.

Implemented:

- standard Arduino `.ino` preprocessing and `setup()`/`loop()` runtime;
- C/C++ global constructors;
- BL616CL board startup and FreeRTOS scheduler;
- GPIO and `LED_BUILTIN` stage-1 mapping;
- `millis()`, `micros()`, `delay()`, and `delayMicroseconds()`;
- console `Serial` on BL616CL DK UART0 (GPIO34 TX / GPIO35 RX, 2 Mbit/s);
- `Serial1` polling UART on GPIO24 TX / GPIO25 RX;
- CherryUSB device support for a CDC ACM + HID composite endpoint, backed by
  the Arduino-style `USBCDC`/`USBHID` compatibility classes;
- C++17 with exceptions and RTTI disabled;
- `.elf`, `.map`, post-processed `.bin`, boot2, partition, and eFuse side cars;
- modern BL616CL `bflb_fw_post_proc` and `BLFlashCommand` integration.

The variant mapping is for compile/bring-up on `bl616cldk`, not the final UNO R4
carrier. GPIO32/33 remain reserved for USB. Confirm the production schematic
before connecting RA4M1 signals. The stage-1 4 MiB partition limits the primary
firmware slot to 2 MiB; eFuse files are exported for traceability but are not
burned by the normal Arduino upload action.

## First-time setup

Clone this repo into `hardware/bouffalo/bl616cl/` inside an Arduino CLI
workspace.  The repo excludes host tools (toolchain, `bflb_fw_post_proc`,
`BLFlashCommand`) via `.gitignore`.  Generate them once:

    python3 hardware/bouffalo/bl616cl/tools/runtime_bundle/generate_runtime_bundle.py \
      --sdk /path/to/bouffalo_sdk \
      --chip bl616cl \
      --toolchain /path/to/Xuantie-900-gcc

Make sure `arduino-cli` is in PATH or set `ARDUINO_CLI=/path/to/arduino-cli`.

## Runtime bundle maintenance

Regenerate when the SDK, `defconfig`, toolchain, or ABI flags change:

    python3 hardware/bouffalo/bl616cl/tools/runtime_bundle/generate_runtime_bundle.py \
      --sdk /home/pfchen/workspace/bouffalo_sdk_gerrit/bouffalo_sdk \
      --chip bl616cl \
      --toolchain /path/to/Xuantie-900-gcc

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

## Compile the UNO R4 bridge skeleton

`libraries/esp32-compat` provides safe-failing ESP32-style headers and symbols
so the upstream `uno-r4-wifi-usb-bridge` sketch can be built against this
platform without modifying the sketch:

    arduino-cli compile --config-file arduino-cli.yaml \
      --fqbn=bouffalo:bl616cl:unor4_bl616cl \
      /path/to/uno-r4-wifi-usb-bridge/UNOR4USBBridge

This milestone proves compile/link compatibility and initializes the BL616CL
USB device controller from the compatibility classes. WiFi, TCP/TLS, storage,
BLE, and OTA operations remain stubs and are not functional on hardware yet;
the USB data path still needs on-board validation.
