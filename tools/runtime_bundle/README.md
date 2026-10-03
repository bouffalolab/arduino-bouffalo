# Bouffalo Arduino runtime builder (multi-chip)

The runtime the Arduino platform links against — SDK headers, generated
`autoconf.h`, SDK archives, the generated linker script, boot2 and DTS — is
**built at Arduino compile time from the git submodule** checked out at
`tools/sdk/bouffalo_sdk`.  No generated headers or archives are checked in:
the submodule is the single source of truth, and every chip's runtime is
derived from it (see "Added a new chip?" below).

`platform.txt` wires the two halves together:

- `recipe.hooks.prebuild.1.pattern` runs `build_sdk_runtime.py` before every
  sketch compile, and
- `compiler.sdk.path` points at `{build.path}/sdk_runtime/{build.mcu}`, the
  per-build location the hook materialises (a symlink into the cache).

## Build flow

1. Fingerprint the inputs and look for a cache entry under
   `~/.cache/arduino-bouffalo/sdk-runtime/<key>` (override with
   `BOUFFALO_SDK_CACHE`; the three newest entries are kept).
2. On a miss the builder copies the probe template (`Makefile`, `main.c`,
   `CMakeLists.txt`, `defconfig`, `FreeRTOSConfig.h`, `usb_config.h`) into a
   work directory, applies the platform patch set to the SDK tree, and builds
   the probe with the SDK's Make/CMake flow using the platform toolchain.
   Vendor archives the public SDK does not publish are staged into the SDK
   tree for the link only.  The patch set is reverted and the vendor archives
   removed in a `finally` block.
3. The staged runtime is published atomically as the cache entry and linked
   into the sketch build directory.

The cache key covers the SDK commit **and dirty state**, the patch set, the
probe `defconfig`/`FreeRTOSConfig.h`, the toolchain version and the SHA-256 of
the vendor archives, so editing any of them rebuilds the runtime.

## Standalone use

    python3 tools/runtime_bundle/build_sdk_runtime.py \
      --platform-root /path/to/hardware/bouffalo/bl616cl \
      --chip bl616cl --board bl616cldk --out /tmp/rt/bl616cl

`--force` rebuilds, `--keep-work` retains the SDK build tree under
`<cache>/work/`, `--no-patches` skips the platform patch set.

`generate_runtime_bundle.py` now only provides the shared helpers plus a
standalone bundle generator writing to an explicit `--out` path (used in the
past for the removed checked-in bundles) and the host toolchain/tool
installer (`--tools-only`); the compile-time path above is the supported
flow.

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

## Added a new chip?

The runtime is generated per chip, so nothing needs to be copied by hand for a
new chip or board: `--chip`/`--board` select the SDK board (`bsp/board/<board>`)
and the toolchain (`tools/{toolchain_dirname}`), and the same probe `defconfig`
drives the build.  A board whose controller variant is not published by the
public SDK additionally needs a vendor archive (see below).

## Patches

SDK fixes are tracked as unified diffs under `patches/`; the builder applies
them to the submodule for the duration of a build and reverts them afterwards.
`patches.json` records the SDK subdirectory each diff applies to and marks
patches that became obsolete upstream.  See `patches/README.md` for the list
and the rationale of each fix.

## Vendor archives

Some prebuilt controller variants are not part of the public SDK release
(BL616CL `uarthci` is one; the v2.3.35 release only ships `m0b1`/`m2s1`).  The
builder accepts such archives from `tools/vendor/bouffalo_ble/<chip>/` and
copies them into the SDK tree for the link only.  Each vendor directory
carries a README and a manifest with the archive version and SHA-256, and the
vendor archive hashes are part of the runtime cache key.

## Legacy checked-in bundles

The `tools/sdk/{chip}/` bundles that predated the submodule flow were removed
on 2026-10-03; `tools/sdk/` now contains only the `bouffalo_sdk` submodule.
`platform.txt` had already stopped reading them.  The BL616CL `uarthci`
controller archive remains under `tools/vendor/bouffalo_ble/` for now because
the public SDK release does not ship that variant.

## Build hygiene

The builder leaves the submodule worktree pristine.  Patches are applied with
`--no-backup-if-mismatch` (GNU patch would otherwise drop `*.orig` files next
to SDK sources whenever a hunk applies with an offset), and the SDK's Python
helpers run with `PYTHONDONTWRITEBYTECODE=1` (the upstream repository tracks
some `__pycache__/*.pyc` files that a build would otherwise rewrite).  The
vendor controller archive is staged into the SDK tree for the link and
removed in a `finally` block.  After reverting the patch set the builder
reports any patch-touched file that is still modified, so drift cannot go
unnoticed.
