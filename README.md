# arduino-bl

[![Source CI](https://github.com/bouffalolab/arduino-bouffalo/actions/workflows/source-ci.yml/badge.svg?branch=master)](https://github.com/bouffalolab/arduino-bouffalo/actions/workflows/source-ci.yml)
[![Firmware CI](https://github.com/bouffalolab/arduino-bouffalo/actions/workflows/firmware-ci.yml/badge.svg?branch=master)](https://github.com/bouffalolab/arduino-bouffalo/actions/workflows/firmware-ci.yml)
[![License: LGPL-2.1](https://img.shields.io/badge/License-LGPL--2.1-blue.svg)](LICENSE)

An Arduino-compatible platform for BouffaloLab devices, starting with the BL616CL used in the Arduino UNO R4 WiFi ecosystem.

## Features

- Arduino-style sketches with Core, board, and library support for BL616CL.
- Installation and builds through the Arduino package workflow without requiring a complete Bouffalo SDK checkout.
- USB serial, hardware UART, GPIO, timing, Wi-Fi, networking, filesystem, and preferences APIs.
- Optional BLE Controller support for applications that provide the host side.
- Versioned prebuilt dependencies supplied by the companion library builder.
- CI coverage for platform descriptors, source contracts, and integrated firmware builds.

The current master line is an initial candidate. The first hardware scope is the Arduino UNO R4 WiFi with a BL616CL module on Linux x86_64; other boards and host systems require separate validation. Check the published support and release notes before using a feature in production.

## Supported boards

| Board | Target MCU | Validation scope |
| --- | --- | --- |
| UNO R4 WiFi (BL616CL coprocessor) | BL616CL | Module 1V0, Linux x86_64; firmware features remain experimental |

This board uses a BL616CL replacement for the original wireless module. Sketches selected for this platform run on BL616CL. RA4M1 sketches use the official Renesas board package. Other module revisions, boards, and chips are not yet validated. See the [support matrix](docs/SUPPORT.md) for feature-specific scope.

## Getting started

### Prerequisites

- Arduino IDE or Arduino CLI.
- A supported Arduino UNO R4 WiFi board with BL616CL.
- The matching `arduino-bl` platform, dependency package, and flash tools from the same release.
- USB access to the board. Linux is required for the currently validated upload and debug workflow.

### Installation

1. Add the package index for the release you want to use, or install the matching packages from a local release bundle.
2. Select the BouffaloLab UNO R4 WiFi board in the Arduino environment.
3. Connect the board over USB and compile a sketch.
4. Upload the generated firmware and open the board's serial monitor.

The platform and dependency packages are versioned as a set. Keep them on the same release when upgrading or rolling back.

## Usage

```cpp
void setup() {
  Serial.begin(115200);
}

void loop() {
  Serial.println("Hello from BouffaloLab");
  delay(1000);
}
```

Applications can include the supported Arduino networking and storage libraries when the matching dependency package is installed. USB, UART, Wi-Fi, BLE Controller, filesystem, and Preferences capabilities have different hardware and version scopes; consult the support documentation for the board and release in use.

## Directory structure

- **Core** — Arduino lifecycle, runtime, USB serial, UART, GPIO, and timing.
- **Libraries** — Wi-Fi, networking, storage, preferences, and optional BLE Controller APIs.
- **Variants** — board pin and resource definitions.
- **Tools and documentation** — package integration, upload support, examples, and support notes.

## Contributing

See the [developer guide](DEVELOP.md) for prerequisites, local setup, builds, tests, and the contribution workflow.

Use GitHub Issues for bugs, missing board support, and documentation gaps. Pull requests should describe the affected board or chip, keep public Arduino behavior compatible, and include the validation scope for any new capability. Changes to platform APIs, board definitions, or dependency requirements should be reviewed together with the matching package and bridge repositories when applicable.

## License and acknowledgement

This repository is released under the [GNU Lesser General Public License v2.1](LICENSE). It incorporates ArduinoCore-API and other third-party components; their copyright and license notices are retained with the corresponding sources and release materials.

The platform is designed to work with [arduino-bl-lib-builder](https://github.com/bouffalolab/arduino-bouffalo-lib-builder) and the [UNO R4 WiFi USB bridge](https://github.com/bouffalolab/arduino-bouffalo-uno-r4-wifi-usb-bridge).
