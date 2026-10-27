# BouffaloLab For Arduino

BouffaloLab For Arduino is an Arduino platform for Bouffalo Lab RISC-V SoCs. It
provides the shared runtime, build system, SDK integration, and board-add model
used by the supported development boards.

Support is organized by SoC and board. This checkout currently contains a
BL616CL core and the first BL616CL board package; additional boards based on
BL616, BL618DG, and other Bouffalo Lab SoCs can be added without changing the
platform architecture.

> **Project status: Stage 1 - active bring-up.**
>
> Supported boards are usable for development and hardware validation, but
> APIs, partition layouts, and board integration may still change.
>
> **Pinned Bouffalo SDK:** `master` commit `1cd1fd5`

## Contents

- [Overview](#overview)
- [Supported boards](#supported-boards)
- [Platform status](#platform-status)
- [Requirements](#requirements)
- [Installation](#installation)
- [Build and upload](#build-and-upload)
- [Board integration](#board-integration)
- [Repository layout](#repository-layout)
- [Runtime build model](#runtime-build-model)
- [Integrations](#integrations)
- [Known limitations](#known-limitations)
- [Documentation](#documentation)
- [Development notes](#development-notes)
- [Third-party components](#third-party-components)

## Overview

This repository provides the BouffaloLab For Arduino platform together with:

- standard `.ino` preprocessing and the `setup()` / `loop()` runtime;
- Bouffalo Lab startup code, FreeRTOS scheduling, GPIO, timing, and UART
  support;
- interrupt-driven `HardwareSerial` instances with board-selected pins;
- CherryUSB device support where the SoC and board expose native USB;
- an `esp32-compat` library that provides selected ESP32-style Arduino APIs;
- a compile-time SDK runtime builder that produces SDK headers, archives, a
  linker script, boot2 data, and DTS data from the pinned SDK submodule;
- partitioned firmware generation and `bflb_fw_post_proc` integration; and
- a board-add interface that keeps the SDK runtime separate from each board's
  Arduino pin mapping.

The platform is installed as a local Arduino hardware package and is not
currently distributed through Arduino Boards Manager.

## Supported boards

The table is the canonical list of boards exposed by this repository. Add new
boards as new rows, with board-specific pin and peripheral details kept in the
corresponding variant or linked board documentation.

| Board | SoC / SDK BSP | Arduino FQBN | On-board hardware | Links |
| --- | --- | --- | --- | --- |
| Ai-Thinker Ai-M64L-32S-Kit | BL616CL / `bl616cldk` | `bouffalo:bl616cl:ai_m64l_32s_kit` | USB-C via CH340; RGB LED and two white LEDs; RESET and FLASH buttons | [Official docs][ai-m64l-docs] / [Taobao][ai-m64l-buy] |

Board-specific mappings live under `variants/<variant>/`. The `build.mcu`,
`build.sdk_board`, and `build.variant` properties keep the selected SoC, SDK BSP,
and Arduino variant independent. Future BL616, BL618DG, and other development
boards will be appended to this table as support lands.

[ai-m64l-docs]: https://docs.ai-thinker.com/en/ai_m64/
[ai-m64l-buy]: <https://item.taobao.com/item.htm?abbucket=3&id=1061659779278&mi_id=00002Q9El4KP5JBO-Ap7S4J81nl0BgBnB6mQwMJEFtIKRp8&ns=1&skuId=6302564583191&spm=a21n57.1.item.1&utparam=%7B%22aplus_abtest%22%3A%22313687401f81c98a1a07e521f276f7c0%22%7D&xxc=taobaoSearch>

## Platform status

Feature status describes the platform implementation and current reference
hardware validation. It is not a guarantee that every peripheral is available
on every SoC or board; use the status in [Supported boards](#supported-boards)
for board-level readiness.

- **Verified** - implemented and exercised on reference hardware.
- **Partial** - implemented, but board integration or broader validation
  remains.
- **Not supported** - only a fail-safe stub or no implementation exists.

| Area | Status | Notes |
| --- | --- | --- |
| Arduino runtime | Verified | `.ino` preprocessing, `setup()` / `loop()`, constructors, `millis()`, `micros()`, and delays. |
| Core peripherals | Verified | GPIO, `LED_BUILTIN`, UART console, and interrupt-driven `HardwareSerial`; exact pins are board-specific. |
| USB device | Partial | CherryUSB support is implemented where the SoC and board expose native USB; some boards use an external USB-UART instead. |
| Wi-Fi STA | Verified | BL616CL reference runtime supports scan, WPA2 connection, DHCP/static IPv4, DNS, and ICMP ping. |
| TCP / UDP | Verified | BL616CL reference runtime exposes `WiFiClient`, `WiFiServer`, and `WiFiUDP` through lwIP. |
| TLS | Verified | BL616CL reference runtime uses mbedTLS 2.28.2; CA loading and HTTPS GET pass. |
| SPIFFS / Preferences | Partial | LittleFS and EasyFlash backends pass `StorageTest`; partition layouts require per-board validation. |
| ESPHome | Experimental | Current BL616CL integration supports the Native API and encrypted network OTA with companion changes. |
| SoftAP / APSTA | Not supported | The current BL616CL Wi-Fi runtime operates in STA mode only. |
| General OTA / update API | Not supported | Generic Arduino update and OTA entry points remain fail-safe stubs. |

## Requirements

The reference development environment is Linux. The platform also contains
tool naming and post-processing support for macOS, but the complete flow must be
validated against the locally installed toolchain.

Prepare:

- `git`;
- Python 3;
- GNU Make and CMake;
- [`arduino-cli`](https://arduino.github.io/arduino-cli/) or Arduino IDE 2.x;
- the RISC-V GCC toolchain required by the selected SoC (for the current
  BL616CL target, the T-Head Xuantie-900 toolchain);
- a board listed in [Supported boards](#supported-boards), or a compatible
  target with a board definition; and
- the power, USB, or USB-UART connection required by that board.

## Installation

### 1. Install the platform in the Arduino sketchbook

Check the Arduino CLI sketchbook location:

```sh
arduino-cli config dump
```

Place this repository under `<sketchbook>/hardware/bouffalo/<architecture>`.
The directory name identifies the Arduino architecture. For the current
`bl616cl` architecture and default Linux sketchbook:

```sh
mkdir -p ~/Arduino/hardware/bouffalo
git clone https://github.com/Virus-V/arduino-bouffalo.git \
  ~/Arduino/hardware/bouffalo/bl616cl
cd ~/Arduino/hardware/bouffalo/bl616cl
```

The top-level project is not limited to the current architecture directory;
additional SoC architectures and boards can be introduced alongside the
existing core.

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
from Git. Install a minimal copy and refresh the host tools with the SoC and SDK
BSP identifiers from the [Supported boards](#supported-boards) table:

```sh
python3 tools/runtime_bundle/generate_runtime_bundle.py \
  --sdk tools/sdk/bouffalo_sdk \
  --chip <soc-id> \
  --board <sdk-bsp> \
  --toolchain <toolchain-root> \
  --tools-only
```

`--toolchain` must point to the toolchain root, the parent directory of `bin/`.
The command writes ignored files under:

```text
tools/<toolchain-directory>/
tools/bflb_fw_post_proc/
tools/bouffalo_flash_cube/
```

The checked-in legacy SDK bundles are not used. The full SDK runtime is built
from the submodule during the first sketch compilation.

### 3. Verify installation

List the available board definitions and copy the FQBN for your board:

```sh
arduino-cli board listall bouffalo
```

## Build and upload

Export the FQBN from the [Supported boards](#supported-boards) table:

```sh
export ARDUINO_FQBN='<fqbn>'
```

Compile the included smoke tests from the platform root:

```sh
arduino-cli compile -b "$ARDUINO_FQBN" examples/Blink
arduino-cli compile -b "$ARDUINO_FQBN" examples/Serial
```

Compile and upload Blink through the port exposed by the board:

```sh
arduino-cli compile --upload \
  -b "$ARDUINO_FQBN" \
  -p /dev/ttyUSB0 \
  examples/Blink
```

Replace `/dev/ttyUSB0` with the board's serial, USB-UART, or native USB port.
Device names can change between plug-in cycles, so check `/dev/serial/by-id/` on
Linux or `/dev/cu.*` on macOS before flashing. Use the board-specific upload
instructions when a board requires a manual boot or flash mode.

Open the console at the rate used by the sketch:

```sh
arduino-cli monitor \
  -b "$ARDUINO_FQBN" \
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

## Board integration

Board metadata is declared in `boards.txt`. Each board selects:

- `build.mcu`: the Bouffalo Lab SoC;
- `build.sdk_board`: the SDK BSP used to build the chip runtime; and
- `build.variant`: the Arduino variant containing board-specific pin aliases.

Arduino pin mappings belong in `variants/<variant>/pins_arduino.h`. Upload
settings, flash layout, and boot assets belong in the board's `boards.txt`
entry and associated platform files. New boards should be added as rows to the
[Supported boards](#supported-boards) table rather than changing the platform
narrative around one product.

## Repository layout

```text
.
|-- boards.txt                 Board definitions and upload settings
|-- platform.txt               Compiler, linker, upload, and prebuild recipes
|-- cores/<soc>/               Arduino runtime and SoC-specific core support
|-- libraries/esp32-compat/    ESP32-style Arduino compatibility layer
|-- variants/<variant>/        Board-specific Arduino pin mappings
|-- examples/                  Blink, Serial, and storage tests
|-- tools/partitions/          Partition-table TOML files
|-- tools/runtime_bundle/      Runtime builder and SDK patches
|-- tools/sdk/bouffalo_sdk/    Pinned Bouffalo SDK submodule
`-- docs/                      SoC, board, integration, and maintenance docs
```

The `tools/<toolchain-directory>`, `tools/bflb_fw_post_proc`, and
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
   submodule, and publishes the runtime atomically; and
4. exposes the runtime to the compiler through
   `{build.path}/sdk_runtime/{build.mcu}`.

Set `BOUFFALO_SDK_CACHE` to move the cache. See
[`tools/runtime_bundle/README.md`](tools/runtime_bundle/README.md) for cache
maintenance, patch behavior, multi-chip settings, and standalone builds.

To force a rebuild and retain the SDK build tree, use the SoC and SDK BSP
identifiers for the selected board:

```sh
python3 tools/runtime_bundle/build_sdk_runtime.py \
  --platform-root . \
  --chip <soc-id> \
  --board <sdk-bsp> \
  --out /tmp/bouffalo-runtime \
  --force \
  --keep-work
```

## Integrations

SoC- and board-specific integrations are documented separately from this
overview so that future targets can be added without changing the primary
workflow.

| Integration | Scope |
| --- | --- |
| [`docs/ESPHOME-BL616CL.md`](docs/ESPHOME-BL616CL.md) | Experimental BL616CL ESPHome implementation, partition profile, and validation notes. |

## Known limitations

- This is a Stage 1 platform, not a production-ready Arduino core.
- Board availability and validation state are tracked in the
  [Supported boards](#supported-boards) table.
- Peripheral support varies by SoC and board; do not assume that a feature
  available on the reference BL616CL target exists on another board.
- The current BL616CL Wi-Fi runtime operates in STA mode only. SoftAP, APSTA,
  IPv6, persistent Wi-Fi settings, and automatic reconnect are not implemented.
- The generic Arduino update and OTA APIs remain fail-safe stubs. ESPHome OTA
  support is an integration-specific path.
- Partition profiles must be validated for each SoC and board. A successful
  small-image storage test does not guarantee that every larger firmware layout
  is safe.
- The current BL616CL ESPHome mDNS path lacks the lwIP IGMP flag; sensors and
  Preferences restore/reset have not been hardware-tested.
- macOS and Windows are not part of the verified reference flow. UART tooling
  and upload-device naming differ between hosts.

## Documentation

| Document | Contents |
| --- | --- |
| [`docs/ESPHOME-BL616CL.md`](docs/ESPHOME-BL616CL.md) | Experimental ESPHome integration and partition profile. |
| [`tools/runtime_bundle/README.md`](tools/runtime_bundle/README.md) | Runtime generation, caching, SDK patches, and toolchain layout. |
| [`docs/CHANGES-non-arduino-bouffalo.md`](docs/CHANGES-non-arduino-bouffalo.md) | Audit trail for SDK patches and related development changes. |

## Development notes

- Keep SoC-specific behavior in `cores/<soc>/` and board-specific pin mappings
  in `variants/<variant>/`.
- Add new boards through `boards.txt`, a variant, and a row in
  [Supported boards](#supported-boards); avoid adding product-specific prose to
  the generic installation and runtime sections.
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
