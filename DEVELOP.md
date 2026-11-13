# Developing arduino-bl

This guide is for contributors working on the Arduino Core, platform libraries, and board variants. For installation and feature availability, start with the [README](README.md), [installation guide](docs/INSTALL.md), and [support matrix](docs/SUPPORT.md).

## Architecture and ownership

A sketch is compiled with the Core and selected platform libraries, then linked with versioned prebuilt dependencies. Normal sketch builds do not build the SDK.

| Repository | Responsibility |
| --- | --- |
| [arduino-bl](https://github.com/bouffalolab/arduino-bouffalo/tree/master) | Arduino API adaptation, Core lifecycle, libraries, variants, and build/upload integration |
| [Library builder](https://github.com/bouffalolab/arduino-bouffalo-lib-builder) | SDK and toolchain locks, profiles, patches, and dependency packages |
| [UNO R4 WiFi bridge](https://github.com/bouffalolab/arduino-bouffalo-uno-r4-wifi-usb-bridge) | RA4M1 protocol, AT/HCI transport, USB application behavior, and CMSIS-DAP |

Keep chip and board behavior separate. General UART, USB, networking, and storage APIs belong here; RA4M1 control and bridge protocol behavior belong in the application.

## Prerequisites

- Linux x86_64; the current hardware baseline is UNO R4 WiFi with BL616CL module 1V0.
- Git and Arduino CLI. The reviewed CLI version is 1.5.1.
- A matching dependency set: currently `bl616cl-libs` 0.12.0, `bl-flash-tools` 0.3.0, and GCC RISC-V 14.3.0 (`gcc-riscv`).
- Python 3 and Bash for source checks; bubblewrap (`bwrap`) for SDK isolation checks.
- USB access and the matching board for runtime validation.

The SDK and a native C/C++ toolchain are needed only when rebuilding dependencies through the builder. The platform's source CI uses Python 3.11; dependency-tool packaging has its own pinned Python environment.

## Local environment

Use a separate sketchbook and CLI configuration so development does not replace an existing Arduino installation. Run the following from a new workspace directory:

```sh
git clone --branch master https://github.com/bouffalolab/arduino-bouffalo.git arduino-bl
mkdir -p dev/sketchbook/hardware/bouffalolab dev/data dev/downloads dev/build
arduino-cli config init --dest-file "$PWD/dev/arduino-cli.yaml"
arduino-cli --config-file "$PWD/dev/arduino-cli.yaml" config set directories.user "$PWD/dev/sketchbook"
arduino-cli --config-file "$PWD/dev/arduino-cli.yaml" config set directories.data "$PWD/dev/data"
arduino-cli --config-file "$PWD/dev/arduino-cli.yaml" config set directories.downloads "$PWD/dev/downloads"
ln -s "$PWD/arduino-bl" dev/sketchbook/hardware/bouffalolab/bl
```

Install the matched packages into that configuration. Replace the example URL with the approved release index or a running local candidate server:

```sh
BL_INDEX_URL="<approved-index-url>"
arduino-cli --config-file "$PWD/dev/arduino-cli.yaml" core update-index --additional-urls "$BL_INDEX_URL"
arduino-cli --config-file "$PWD/dev/arduino-cli.yaml" core install bouffalolab:bl@0.2.0 --additional-urls "$BL_INDEX_URL"
```

Use the release index from the [installation guide](docs/INSTALL.md), or use the builder's [development workflow](https://github.com/bouffalolab/arduino-bouffalo-lib-builder/blob/master/DEVELOP.md) to generate local dependencies and compile against explicit package paths.

The sketchbook symlink makes the source checkout the active development platform. Check verbose compiler output when changing configurations to confirm which Core and dependency paths are selected.

## Build and upload

From the workspace directory used above:

```sh
arduino-cli --config-file "$PWD/dev/arduino-cli.yaml" compile \
  -b bouffalolab:bl:unor4wifi_bl616cl \
  --build-path "$PWD/dev/build/Blink" arduino-bl/tests/sketches/Blink
```

This FQBN programs BL616CL. UNO header pins belong to RA4M1; the BL variant defines the coprocessor GPIO mapping.

For uploads, identify the correct board and close only the serial sessions you own. Resolve a stable device path to its current tty before passing it to the uploader. Use the [recovery guide](docs/RECOVERY.md) when the bridge owns the USB touch behavior or the device is already in ROM download mode.

```sh
BL_PORT="$(readlink -f /dev/serial/by-path/YOUR_DEVICE)"
arduino-cli --config-file "$PWD/dev/arduino-cli.yaml" upload \
  -b bouffalolab:bl:unor4wifi_bl616cl -p "$BL_PORT" \
  --input-dir "$PWD/dev/build/Blink" arduino-bl/tests/sketches/Blink
```

Uploading a platform sketch replaces the running BL bridge. Retain the original firmware and persistent-data backup when needed; changing partition layouts requires an explicit migration plan.

## Directory structure

| Location | Responsibility |
| --- | --- |
| `cores/bouffalo/` | Core entry, C++ runtime, GPIO, clocks, UART, USB, and the retained ArduinoCore-API sources |
| `libraries/` | Wi-Fi/networking, LittleFS, Preferences, and BLE Controller interfaces and examples |
| `variants/` | Board pin/resource mapping |
| `boards.txt`, `platform.txt` | Board selection and Arduino build/upload recipes |
| `tools/` | Host integration, including Linux USB permissions |
| `tests/` | Sketch compilation, map checks, and image checks |
| `docs/` | Installation, support, recovery, and license information |
| `.github/workflows/` | Source checks and integrated firmware builds |

## Testing

### Source and build checks

Run source checks from this repository root:

```sh
bash -n tests/compile_check.sh
python3 -I -m compileall -q tests
git diff --check
```

For the platform test sketches, supply the exported library package, toolchain root, and flash-tool package. Additional arguments identify SDK/build directories to hide during compilation:

```sh
tests/compile_check.sh /absolute/path/to/bl616cl-libs \
  /absolute/path/to/gcc-riscv-toolchain /absolute/path/to/bl-flash-tools \
  /absolute/path/to/sdk /absolute/path/to/sdk-build
```

This uses a temporary sketchbook and checks the linker map and generated image. Arduino CLI may still need network access for built-in tools. Installation, relocation, read-only package, and Core override checks are provided by the builder's `tools/install_check.sh`; they test the archived platform HEAD rather than uncommitted edits.

### Hardware acceptance

Only real hardware establishes functional acceptance. Record the board/module revision, wiring, source and package identities, firmware hash, partition layout, device identity, test configuration, expected behavior, and raw logs.

Report source checks, compilation, image generation, flashing, serial runtime, and end-to-end results separately. Software or fake tests cannot establish wireless behavior or persistent storage. Mark capabilities `supported`, `experimental`, or `unimplemented` according to the [support definitions](docs/SUPPORT.md).

## Development and contribution workflow

1. Read the affected API, implementation, immediate caller, and shared interfaces.
2. Reproduce a behavior defect before changing it. Preserve Arduino API semantics and keep changes scoped.
3. Implement general capabilities in the Core or libraries; add board wiring through variants. New chips also require startup, ABI, driver, and dependency validation.
4. Run the relevant source/build checks and hardware scenarios. Report unavailable checks and limits explicitly.
5. Update public documentation and support scope. Submit a PR describing the problem, solution, compatibility impact, and evidence.

Use English for code comments. Document public C/C++ functions with Doxygen-style brief, parameters, return values, and relevant ownership/context constraints. Preserve upstream notices when modifying third-party code.

## Dependency changes and releases

A Core-only change should reuse the existing dependency package. SDK configuration, ABI, libraries, headers, linker resources, and tool changes must be handled through the builder and released as a matching set.

Firmware CI compiles representative platform sketches and the bridge; it does not flash hardware or publish packages. Releases require reviewed versions, licenses, exact source/package identities, and hardware scope. Follow the builder's [controlled release procedure](https://github.com/bouffalolab/arduino-bouffalo-lib-builder/blob/master/docs/releasing.md).
