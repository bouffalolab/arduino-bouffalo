# Bouffalo Arduino bundle builder (multi-chip)

This directory is the only place where the Arduino platform invokes the
Bouffalo SDK build system. Normal Arduino builds compile the sketch, libraries,
and Core directly, then link the checked-in bundles:

- `tools/sdk/{chip}/`: chip/runtime headers, generated `autoconf.h`, SDK
  archives, the generated linker script, and the chip-runtime manifest;
- `variants/{board}/`: board headers, `libapp.a`, pin map, boot2, partition
  configuration/binary, eFuse sidecars, and board manifest;
- `tools/{toolchain_dirname}/`: minimal toolchain subset for the chip.

This follows the same high-level model as Arduino ESP32's `tools/sdk/<chip>`.

## Runtime Profiles

`--profile stage1` is the default lightweight Arduino runtime. `--profile
bridge` emits a sibling `tools/sdk/bl616cl/bridge/` bundle for the provisional
UNO R4 BL616CL bridge FQBN. The bridge profile includes Wi-Fi/MACSW/FHOST, BLE,
CherryUSB, LittleFS/EasyFlash, lwIP and mbedTLS archives and headers.

The generator also copies SDK linker-script `INCLUDE` fragments, bridge
external archives built under `build_macsw/` and `build_fhost/`, and the GCC
LTO executables required by MACSW's `-flto -ffat-lto-objects` archives.

## Supported chips

| Chip | Toolchain prefix | Core |
|------|-----------------|------|
| bl616cl | `riscv64-unknown-elf-` | T-Head E907 |
| bl616 | `riscv64-unknown-elf-` | T-Head E907 |
| bl618dg | `riscv64-zephyr-elf-` | T-Head E907 |
| bl602 | `riscv64-unknown-elf-` | SiFive E24 |
| bl702 | `riscv64-unknown-elf-` | T-Head E907 |
| bl702l | `riscv64-unknown-elf-` | T-Head E907 |

See `CHIP_CONFIG` in `generate_runtime_bundle.py` for the full per-chip
settings (ABI flags, MTIME addresses, FreeRTOS extension directory).

## SDK patches

Some checked-in archives are built from patched SDK sources.  Local SDK fixes
are tracked as unified diffs under `patches/`; see `patches/README.md` for the
list and apply them to a fresh SDK checkout before regenerating a bundle.

## Regenerate

No SDK commit is hardcoded — the generator records whatever commit is currently
checked out.  Uncommitted source changes produce a warning and a `-dirty`
suffix in the manifest.  By default, the generator now rejects dirty SDK
inputs.  Pass `--allow-dirty-sdk` only for a development bundle that must be
tested before the SDK worktree is clean; such a bundle is not release
reproducible.

From the repository root:

    # For BL616CL (default Xuantie-900 toolchain)
    python3 hardware/bouffalo/bl616cl/tools/runtime_bundle/generate_runtime_bundle.py \
      --sdk /path/to/bouffalo_sdk \
      --chip bl616cl

    # For BL618DG (Zephyr toolchain required)
    python3 hardware/bouffalo/bl616cl/tools/runtime_bundle/generate_runtime_bundle.py \
      --sdk /path/to/bouffalo_sdk \
      --chip bl618dg \
      --toolchain /opt/riscv64-zephyr-elf

    # Custom board
    python3 hardware/bouffalo/bl616cl/tools/runtime_bundle/generate_runtime_bundle.py \
      --sdk /path/to/bouffalo_sdk \
      --chip bl616cl \
      --board my_custom_board

    # BL616CL provisional bridge runtime
    python3 hardware/bouffalo/bl616cl/tools/runtime_bundle/generate_runtime_bundle.py \
      --sdk /path/to/bouffalo_sdk \
      --chip bl616cl \
      --profile bridge \
      --toolchain /path/to/Xuantie-900-gcc

Do not pass a toolchain path that lies inside the platform `tools/` directory —
the generator atomically replaces that target.

The generator builds a minimal probe (`main.c` → empty `while(1)`) to produce
all required archives and side-car binaries in one shot, validates the ELF
header, copies only versioned FlashCube chip resources (not `img_create`,
generated `.ini`, or logs), and writes SHA-256 manifests.  Regenerate and
review manifests whenever the SDK commit, `defconfig`, toolchain, ABI flags, or
partition layout change.

For the bridge profile, the generator applies explicitly named compatibility
fixes only to copied public headers in the generated bundle. The external
Bouffalo SDK checkout is never modified.

## Verify a bundle

The bridge bundle verifier checks every manifest file hash and size, the
bridge `defconfig`, the required Wi-Fi/MACSW/FHOST/lwIP/WPA and BLE archives,
the public headers, and (when `--sdk` is supplied) the recorded SDK and build
input sub-repository commits. It also checks
`source_manifest.json` and `proprietary_manifest.json`: every bridge link
archive must be classified exactly once as a public-source build input or a
supplier-prebuilt artifact, while BL616CL ROM ABI dependencies are explicit.
The recorded source repositories include mbedTLS, LittleFS, lwIP, CherryUSB,
Bluetooth, Wi-Fi6/MACSW, LHAL, BL616CL PHY/std, system, and post-processing
tools:

    python3 hardware/bouffalo/bl616cl/tools/runtime_bundle/verify_runtime_bundle.py \
      --bundle hardware/bouffalo/bl616cl/tools/sdk/bl616cl/bridge \
      --sdk /path/to/bouffalo_sdk

The verifier rejects `-dirty` source records by default.  For development-only
inspection of the current provisional bundle:

    python3 hardware/bouffalo/bl616cl/tools/runtime_bundle/verify_runtime_bundle.py \
      --allow-dirty \
      --sdk /path/to/bouffalo_sdk

The release gate is the first command without `--allow-dirty`; it must print
`SOURCE_REPRODUCIBILITY=REPRODUCIBLE` and
`BL616CL_RUNTIME_BUNDLE_VERIFY_PASS`. `BL616CL_SOURCE_BOUNDARY_VERIFY_PASS`
means that the binary boundary is auditable; it does not approve supplier
binary redistribution. The final product still needs a pinned SDK source
snapshot or submodule, supplier license confirmation, and a clean regeneration
before publication.
