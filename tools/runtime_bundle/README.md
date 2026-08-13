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
suffix in the manifest.

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

Do not pass a toolchain path that lies inside the platform `tools/` directory —
the generator atomically replaces that target.

The generator builds a minimal probe (`main.c` → empty `while(1)`) to produce
all required archives and side-car binaries in one shot, validates the ELF
header, copies only versioned FlashCube chip resources (not `img_create`,
generated `.ini`, or logs), and writes SHA-256 manifests.  Regenerate and
review manifests whenever the SDK commit, `defconfig`, toolchain, ABI flags, or
partition layout change.
