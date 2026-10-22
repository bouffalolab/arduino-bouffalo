# BouffaloLab For Arduino

BouffaloLab For Arduino is an Arduino platform project for Bouffalo Lab RISC-V
SoCs. The current implementation provides a BL616CL port, including startup
code, an Arduino-compatible runtime, core peripherals, and an ESP32-style
compatibility library.

> **Project status: Stage 1 - active bring-up.**
>
> The current BL616CL target is usable for development and hardware validation,
> but APIs, partition layouts, and board integration may still change.
>
> **Pinned Bouffalo SDK:** `master` commit `1cd1fd5`

## Contents

- [Overview](#overview)
- [Current target](#current-target)
- [Feature status](#feature-status)
- [Requirements](#requirements)
- [Installation](#installation)
- [Build and upload](#build-and-upload)
- [Pin map](#pin-map)
- [Repository layout](#repository-layout)
- [Runtime build model](#runtime-build-model)
- [ESPHome integration](#esphome-integration)
- [Known limitations](#known-limitations)
- [Documentation](#documentation)
- [Development notes](#development-notes)
- [Third-party components](#third-party-components)

## Overview

This repository provides the BouffaloLab For Arduino platform together with:

- standard `.ino` preprocessing and the `setup()` / `loop()` runtime;
- BL616CL startup, FreeRTOS scheduling, GPIO, timing, and UART support;
- interrupt-driven `HardwareSerial` instances with configurable pins;
- CherryUSB device support;
- an `esp32-compat` library that provides selected ESP32-style Arduino APIs;
- a compile-time SDK runtime builder that produces SDK headers, archives, a
  linker script, boot2 data, and DTS data from the pinned SDK submodule;
- partitioned firmware generation and `bflb_fw_post_proc` integration.

Bouffalo Lab device support is the project scope; this checkout currently
exposes BL616CL board definitions. The platform is installed as a local Arduino
hardware package and is not currently distributed through Arduino Boards
Manager.

## Current target

The current platform package identifier is `bouffalo:bl616cl`. List the
installed board definitions and their FQBNs with:

```sh
arduino-cli board listall bouffalo
```

| Item | Value |
| --- | --- |
| SoC | Bouffalo Lab BL616CL |
| CPU | T-Head E907, `rv32imafc_xtheade`, `ilp32f` |
| Arduino architecture | `bl616cl` |
| Variant | `bl616cldk` |
| Flash profile | 4 MiB |
| Maximum application slot | 2 MiB |
| SRAM limit | 320 KiB (384 KiB minus a 64 KiB Wi-Fi reservation) |
| Console / ISP UART | `Serial`, GPIO34 TX / GPIO35 RX, 2 Mbit/s |
| Secondary UART | `Serial1`, GPIO6 TX / GPIO7 RX |
| Built-in LED | GPIO31 |
| Upload interface | UART through Bouffalo `BLFlashCommand` |

A BL616CL DK is sufficient for core, networking, USB, and storage bring-up.
Support for additional Bouffalo Lab SoCs will require their corresponding
cores, variants, and board definitions.

## Feature status

Status meanings:

- **Verified** - implemented and exercised on hardware.
- **Partial** - implemented, but integration details or broader validation
  remain.
- **Not supported** - only a fail-safe stub or no implementation exists.

| Area | Status | Notes |
| --- | --- | --- |
| Arduino runtime | Verified | `.ino` preprocessing, `setup()` / `loop()`, constructors, `millis()`, `micros()`, and delays. |
| Core peripherals | Verified | GPIO, `LED_BUILTIN`, UART console, and interrupt-driven `HardwareSerial`. |
| USB device | Verified | CherryUSB enumeration and CDC ACM data transfer. |
| Wi-Fi STA | Verified | Scan, WPA2 connection, DHCP/static IPv4, DNS, and ICMP ping. |
| TCP / UDP | Verified | `WiFiClient`, `WiFiServer`, and `WiFiUDP` use the lwIP socket backend. |
| TLS | Verified | `WiFiClientSecure` uses the SDK's mbedTLS 2.28.2; CA loading and HTTPS GET pass. |
| SPIFFS / Preferences | Partial | LittleFS and EasyFlash backends pass `StorageTest`; production partition layouts require application-specific validation. |
| ESPHome | Experimental | API, encrypted control, and network OTA were verified with companion ESPHome changes. |
| SoftAP / APSTA | Not supported | Wi-Fi currently operates in STA mode only. |
| General OTA / update API | Not supported | The generic Arduino update and OTA entry points remain fail-safe stubs. |

## Requirements

The reference development environment is Linux. The platform also contains
tool naming and post-processing support for macOS, but the complete flow must be
validated against the locally installed toolchain.

Prepare:

- `git`;
- Python 3;
- GNU Make and CMake;
- [`arduino-cli`](https://arduino.github.io/arduino-cli/) or Arduino IDE 2.x;
- a T-Head Xuantie-900 GCC toolchain providing `riscv64-unknown-elf-gcc`;
- a BL616CL DK or compatible BL616CL target;
- for UART upload, a USB-UART adapter connected to the BL616CL console and ISP
  pins.

## Installation

### 1. Install the platform in the Arduino sketchbook

Check the Arduino CLI sketchbook location:

```sh
arduino-cli config dump
```

Place this repository under `<sketchbook>/hardware/bouffalo/bl616cl` so Arduino
can discover the current architecture. For the default Linux location:

```sh
mkdir -p ~/Arduino/hardware/bouffalo
git clone https://github.com/Virus-V/arduino-bouffalo.git \
  ~/Arduino/hardware/bouffalo/bl616cl
cd ~/Arduino/hardware/bouffalo/bl616cl
```

The final `bl616cl` directory and package identifiers match the current Arduino
architecture; they do not limit the project to a single Bouffalo Lab SoC.

The SDK submodule URL in `.gitmodules` uses SSH. If SSH access to GitHub is
not configured, initialize the submodule through HTTPS instead:

```sh
git config submodule.tools/sdk/bouffalo_sdk.url \
  https://github.com/bouffalolab/bouffalo_sdk.git
git submodule update --init --recursive
```

If `arduino-cli.yaml` uses a non-default sketchbook, install the repository in
that `directories.user` path instead. Add `--config-file /path/to/arduino-cli.yaml`
to the commands below when using a custom configuration.

### 2. Install the host tools

The toolchain, post-processor, and flash command are intentionally excluded
from Git. Install a minimal copy and refresh the host tools with:

```sh
python3 tools/runtime_bundle/generate_runtime_bundle.py \
  --sdk tools/sdk/bouffalo_sdk \
  --chip bl616cl \
  --board bl616cldk \
  --toolchain /path/to/Xuantie-900-gcc \
  --tools-only
```

`--toolchain` must point to the toolchain root, the parent directory of `bin/`.
The command writes ignored files under:

```text
tools/Xuantie-900-gcc/
tools/bflb_fw_post_proc/
tools/bouffalo_flash_cube/
```

The checked-in legacy SDK bundles are not used. The full SDK runtime is built
from the submodule during the first sketch compilation.

### 3. Verify installation

List the available board definitions:

```sh
arduino-cli board listall bouffalo
```

Use the reported FQBN in the build commands below.

## Build and upload

Set `BL616CL_FQBN` to the FQBN reported by `arduino-cli board listall`:

```sh
export BL616CL_FQBN='bouffalo:bl616cl:<board-id>'
```

Compile the included smoke tests from the platform root:

```sh
arduino-cli compile -b "$BL616CL_FQBN" examples/Blink
arduino-cli compile -b "$BL616CL_FQBN" examples/Serial
```

Compile and upload Blink:

```sh
arduino-cli compile --upload \
  -b "$BL616CL_FQBN" \
  -p /dev/ttyUSB0 \
  examples/Blink
```

Replace `/dev/ttyUSB0` with the BL616CL console and ISP UART on your system.
The serial device name can change between plug-in cycles, so check
`/dev/serial/by-id/` on Linux or `/dev/cu.*` on macOS before flashing.

Open the console at the rate used by the sketch:

```sh
arduino-cli monitor \
  -b "$BL616CL_FQBN" \
  -p /dev/ttyUSB0 \
  -c baudrate=2000000
```

The normal upload path writes the application image and exports boot2,
partition, eFuse, `.elf`, and `.map` side files for inspection. It does not
burn eFuse data.

### Examples

| Example | Purpose |
| --- | --- |
| `examples/Blink` | GPIO, timing, and UART console smoke test. |
| `examples/Serial` | Console output and C++ global constructor check. |
| `examples/StorageTest` | LittleFS/SPIFFS and EasyFlash-backed Preferences checks. |

## Pin map

The current `bl616cldk` variant provides this Arduino mapping:

| Signal | BL616CL pin | Purpose |
| --- | --- | --- |
| `LED_BUILTIN` | GPIO31 | On-board LED. |
| `Serial` TX / RX | GPIO34 / GPIO35 | UART0 console and ISP. |
| `Serial1` TX / RX | GPIO6 / GPIO7 | UART1. |
| `SDA` / `SCL` | GPIO11 / GPIO14 | Variant I2C mapping. |

GPIO32 and GPIO33 are reserved for USB. `Serial2` is not provided by the
current variant.

## Repository layout

```text
.
|-- boards.txt                 Board definitions and upload settings
|-- platform.txt               Compiler, linker, upload, and prebuild recipes
|-- cores/bl616cl/             Arduino runtime and BL616CL core
|-- libraries/esp32-compat/    ESP32-style Arduino compatibility layer
|-- variants/bl616cldk/        Arduino pin mapping
|-- examples/                  Blink, Serial, and storage tests
|-- tools/partitions/          Partition-table TOML files
|-- tools/runtime_bundle/      Runtime builder and SDK patches
|-- tools/sdk/bouffalo_sdk/    Pinned Bouffalo SDK submodule
`-- docs/                      ESPHome and maintenance documentation
```

The `tools/Xuantie-900-gcc`, `tools/bflb_fw_post_proc`, and
`tools/bouffalo_flash_cube` directories are generated locally and ignored by
Git.

## Runtime build model

`platform.txt` runs `tools/runtime_bundle/build_sdk_runtime.py` through a
prebuild hook before each sketch compile. The builder:

1. fingerprints the SDK commit and dirty state, patches, configuration files,
   toolchain, and optional controller archive;
2. reuses a cached runtime under
   `~/.cache/arduino-bouffalo/sdk-runtime/` when possible;
3. otherwise applies the platform patch set, builds the SDK probe, restores the
   submodule, and publishes the runtime atomically;
4. exposes the runtime to the compiler through
   `{build.path}/sdk_runtime/{build.mcu}`.

Set `BOUFFALO_SDK_CACHE` to move the cache. See
[`tools/runtime_bundle/README.md`](tools/runtime_bundle/README.md) for cache
maintenance, patch behavior, multi-chip settings, and standalone builds.

To force a rebuild and retain the SDK build tree:

```sh
python3 tools/runtime_bundle/build_sdk_runtime.py \
  --platform-root . \
  --chip bl616cl \
  --board bl616cldk \
  --out /tmp/bl616cl-runtime \
  --force \
  --keep-work
```

## ESPHome integration

The experimental ESPHome BL616CL integration uses this Arduino checkout with a
companion ESPHome platform implementation and
`tools/partitions/partition_cfg_esphome_4M.toml`.

Validated on 2026-10-18:

- Wi-Fi connection to a WPA3-SAE AP with protected management frames;
- DHCP and encrypted ESPHome Native API;
- control from Home Assistant;
- encrypted OTA image validation, commit, and reboot.

Factory images must be written as raw bytes at address `0`. Do not send a
factory image to network OTA. See
[`docs/ESPHOME-BL616CL.md`](docs/ESPHOME-BL616CL.md) for branch requirements,
partition details, and current validation gaps.

## Known limitations

- This is a Stage 1 platform, not a production-ready Arduino core.
- Only the checked-in board definitions are currently exposed.
- Wi-Fi supports STA mode only; SoftAP, APSTA, IPv6, persistent Wi-Fi settings,
  and automatic reconnect are not implemented.
- The generic Arduino update and OTA APIs remain fail-safe stubs. ESPHome OTA
  support is an integration-specific path.
- The current partition profile must be validated for each deployment; a
  successful small-image storage test does not guarantee that every larger
  firmware layout is safe.
- ESPHome mDNS stalls because the current wl80211 lwIP netif lacks the IGMP
  flag. Sensors and Preferences restore/reset have not been hardware-tested.
- macOS and Windows are not part of the verified reference flow. UART tooling
  and upload-device naming differ between hosts.

## Documentation

| Document | Contents |
| --- | --- |
| [`docs/ESPHOME-BL616CL.md`](docs/ESPHOME-BL616CL.md) | Experimental ESPHome integration and partition profile. |
| [`tools/runtime_bundle/README.md`](tools/runtime_bundle/README.md) | Runtime generation, caching, SDK patches, and toolchain layout. |
| [`docs/CHANGES-non-arduino-bouffalo.md`](docs/CHANGES-non-arduino-bouffalo.md) | Audit trail for SDK patches and related development changes. |

## Development notes

- Keep SoC-specific behavior in the matching core and variant; BL616CL-specific
  code belongs under `cores/bl616cl` and `variants/bl616cldk`.
- Put SDK fixes in `tools/runtime_bundle/patches/` and describe their purpose,
  upstream status, and validation in the patch documentation.
- Do not commit generated runtime bundles, toolchains, flash logs, or build
  output.
- Record hardware evidence when changing clocks, pins, partitions, USB, RF,
  or low-level timing behavior.
- Run the Blink, Serial, and StorageTest builds before submitting integration
  changes. Hardware validation is preferred for peripheral, network, and
  power-management changes.

## Third-party components

This repository contains or fetches Bouffalo SDK components, FreeRTOS, lwIP,
mbedTLS, CherryUSB, and other third-party sources. Those components remain
subject to their own upstream licenses and notices. The repository currently
does not declare a separate top-level license.
