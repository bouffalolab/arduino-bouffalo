#!/usr/bin/env python3
"""Build Bouffalo Arduino SDK/variant bundles from a BouffaloSDK checkout.

Generates the checked-in runtime bundles used by the Arduino platform:
  tools/sdk/{chip}/          chip-level headers, archives, linker script
  variants/{board}/           board BSP archive, boot2, partition, eFuse assets
  tools/{toolchain_dirname}/  minimal toolchain subset

Example:
    python3 generate_runtime_bundle.py \\
      --sdk /path/to/bouffalo_sdk \\
      --chip bl616cl \\
      --toolchain /opt/Xuantie-900-gcc

Chip-specific settings (toolchain prefix, ABI flags, toolchain directory name,
FreeRTOS MTIME addresses) are drawn from the CHIP_CONFIG table below.  Add
entries there when bringing up a new SoC.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
from pathlib import Path

# ---------------------------------------------------------------------------
# Per-chip configuration
# ---------------------------------------------------------------------------
#
# toolchain_prefix   GCC triplet prefix (e.g. riscv64-unknown-elf)
# toolchain_dirname  subdirectory under tools/ that holds the minimal toolchain
# arch               ISA / ABI / tuning flags (recorded in manifests)
# mtime_base         FreeRTOS configMTIME_BASE_ADDRESS for this chip
# mtimecmp_base      FreeRTOS configMTIMECMP_BASE_ADDRESS for this chip
# freertos_extension chip_specific_extensions subdirectory (None → skip)
# ---------------------------------------------------------------------------

CHIP_CONFIG: dict[str, dict[str, object]] = {
    "bl616cl": {
        "toolchain_prefix": "riscv64-unknown-elf",
        "toolchain_dirname": "Xuantie-900-gcc",
        "arch": {
            "march": "rv32imafc_xtheade",
            "mabi": "ilp32f",
            "mtune": "e907",
        },
        "mtime_base": "0xE000BFF8UL",
        "mtimecmp_base": "0xE0004000UL",
        "freertos_extension": "RV32I_CLINT_no_extensions",
    },
    "bl616": {
        "toolchain_prefix": "riscv64-unknown-elf",
        "toolchain_dirname": "Xuantie-900-gcc",
        "arch": {
            "march": "rv32imafc_xtheade",
            "mabi": "ilp32f",
            "mtune": "e907",
        },
        "mtime_base": "0xE000BFF8UL",
        "mtimecmp_base": "0xE0004000UL",
        "freertos_extension": "RV32I_CLINT_no_extensions",
    },
    "bl618dg": {
        "toolchain_prefix": "riscv64-zephyr-elf",
        "toolchain_dirname": "Xuantie-900-gcc-zephyr",
        "arch": {
            "march": "rv32imafc_xtheade",
            "mabi": "ilp32f",
            "mtune": "e907",
        },
        "mtime_base": "0xE000BFF8UL",
        "mtimecmp_base": "0xE0004000UL",
        "freertos_extension": "RV32I_CLINT_no_extensions",
    },
    "bl602": {
        "toolchain_prefix": "riscv64-unknown-elf",
        "toolchain_dirname": "Xuantie-900-gcc",
        "arch": {
            "march": "rv32imac",
            "mabi": "ilp32",
            "mtune": "e24",
        },
        "mtime_base": "0x0200BFF8UL",
        "mtimecmp_base": "0x02004000UL",
        "freertos_extension": "RISCV_no_extensions",
    },
    "bl702": {
        "toolchain_prefix": "riscv64-unknown-elf",
        "toolchain_dirname": "Xuantie-900-gcc",
        "arch": {
            "march": "rv32imafc",
            "mabi": "ilp32f",
            "mtune": "e907",
        },
        "mtime_base": "0x0200BFF8UL",
        "mtimecmp_base": "0x02004000UL",
        "freertos_extension": "RISCV_MTIME_CLINT_no_extensions",
    },
    "bl702l": {
        "toolchain_prefix": "riscv64-unknown-elf",
        "toolchain_dirname": "Xuantie-900-gcc",
        "arch": {
            "march": "rv32imafc",
            "mabi": "ilp32f",
            "mtune": "e907",
        },
        "mtime_base": "0x0200BFF8UL",
        "mtimecmp_base": "0x02004000UL",
        "freertos_extension": "RISCV_MTIME_CLINT_no_extensions",
    },
}

# Archives the SDK build is expected to produce for every chip.
EXPECTED_SDK_ARCHIVES = {
    "libfreertos.a",
    "liblhal.a",
    "liblibc.a",
    "libmm.a",
    "libstd.a",
    "libsys.a",
    "libsysinit.a",
    "libutils.a",
}
EXPECTED_BOARD_ARCHIVE = "libapp.a"

# Side-car binaries that bflb_fw_post_proc must produce.
EXPECTED_SIDE_CARS = {
    "partition.bin",
    "efusedata.bin",
    "efusedata_mask.bin",
    "efusedata_raw.bin",
}


# ===================================================================
# Utilities
# ===================================================================


def run(command: list[str], *, cwd: Path | None = None,
        env: dict[str, str] | None = None, capture: bool = False) -> str:
    print("+", " ".join(str(value) for value in command))
    result = subprocess.run(
        [str(value) for value in command],
        cwd=cwd, env=env, check=True, text=True,
        stdout=subprocess.PIPE if capture else None,
        stderr=subprocess.STDOUT if capture else None,
    )
    return result.stdout.strip() if capture else ""


def require_file(path: Path, description: str) -> Path:
    if not path.is_file():
        raise RuntimeError(f"missing {description}: {path}")
    return path


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def copy_file(source: Path, destination: Path, *,
              executable: bool = False) -> None:
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, destination)
    if executable:
        destination.chmod(destination.stat().st_mode | 0o111)


def tracked_source_is_dirty(repository: Path) -> bool:
    status = subprocess.run(
        ["git", "-C", str(repository),
         "status", "--porcelain=v1", "--untracked-files=no"],
        check=True, text=True, stdout=subprocess.PIPE,
    ).stdout
    return bool(status.strip())


def copy_headers(source_roots: list[tuple[Path, Path]],
                 include_root: Path) -> None:
    for source_root, relative_root in source_roots:
        if not source_root.is_dir():
            raise RuntimeError(f"missing include directory: {source_root}")
        for source in sorted(source_root.rglob("*.h")):
            destination = (include_root / relative_root /
                           source.relative_to(source_root))
            copy_file(source, destination)


def sdk_version(sdk: Path) -> str:
    version_file = sdk / "VERSION"
    if version_file.is_file():
        match = re.search(
            r'PROJECT_SDK_VERSION\s+"([^"]+)"',
            version_file.read_text(encoding="utf-8"),
        )
        if match:
            return match.group(1)
    return run(
        ["git", "-C", str(sdk), "describe", "--tags", "--always",
         "--long", "--match", "[0-9]*"],
        capture=True,
    )


def resolve_toolchain(toolchain_arg: Path | None,
                      chip_cfg: dict[str, object]) -> Path:
    """Locate the toolchain root (parent of bin/)."""
    prefix = str(chip_cfg["toolchain_prefix"])
    if toolchain_arg:
        root = toolchain_arg.expanduser().resolve()
    else:
        gcc_name = f"{prefix}-gcc"
        located = shutil.which(gcc_name)
        if not located:
            raise RuntimeError(
                f"{gcc_name} not in PATH; pass --toolchain /path/to/toolchain"
            )
        root = Path(located).resolve().parent.parent  # bin/ → root
    require_file(root / "bin" / f"{prefix}-gcc", f"{prefix}-gcc")
    return root


def _find_best_multilib(base: Path, march: str, mabi: str) -> tuple[str, str]:
    """Find the best multilib/ target-lib directory under *base*.

    Returns (relative_path, mabi_subdir).  Prefers exact match, then
    rv32 superset of the requested march, then any rv32 match.
    """
    exact = f"{march}/{mabi}"
    if (base / exact).is_dir():
        return exact, mabi

    # Collect all rv32 arch directories (each may contain multiple mabi subdirs)
    rv32_arches: dict[str, list[str]] = {}
    for d in sorted(base.iterdir()):
        if not d.is_dir() or d.name in ("include", "include-fixed", "."):
            continue
        if not d.name.startswith("rv32"):
            continue
        subdirs = sorted(
            sd.name for sd in d.iterdir() if sd.is_dir()
        )
        if subdirs:
            rv32_arches[d.name] = subdirs

    if not rv32_arches:
        # No rv32 at all — pick last available (may be rv64)
        candidates = sorted(
            p.name for p in base.iterdir()
            if p.is_dir() and p.name not in ("include", "include-fixed")
        )
        if not candidates:
            raise RuntimeError(f"no multilib directories found under {base}")
        best_arch = candidates[-1]
        subdirs = sorted(
            sd.name for sd in (base / best_arch).iterdir() if sd.is_dir()
        )
        best_mabi = subdirs[-1] if subdirs else "."
        result = f"{best_arch}/{best_mabi}"
        print(f"Note: no rv32 multilib in {base}; "
              f"using {result}", file=sys.stderr)
        return result, best_mabi

    # Prefer exact mabi match in any rv32 arch.
    for arch in sorted(rv32_arches):
        if mabi in rv32_arches[arch]:
            result = f"{arch}/{mabi}"
            print(f"Note: multilib {exact} not found; using {result}",
                  file=sys.stderr)
            return result, mabi

    # No exact mabi — pick arch with most mabi options, its last mabi.
    best_arch = max(rv32_arches, key=lambda a: len(rv32_arches[a]))
    best_mabi = rv32_arches[best_arch][-1]
    result = f"{best_arch}/{best_mabi}"
    print(f"Note: multilib {exact} not found; using {result}",
          file=sys.stderr)
    return result, best_mabi


def copy_minimal_toolchain(toolchain_root: Path, destination: Path,
                           prefix: str, arch: dict[str, object]) -> str:
    """Copy only the files needed by Arduino compile/link recipes.

    Uses the chip's arch.march / arch.mabi to select the correct
    multilib and target-library directories.

    Returns the GCC version string for manifest recording.
    """
    root = toolchain_root.resolve()
    destination = destination.resolve()
    if root == destination or destination in root.parents or \
       root in destination.parents:
        raise RuntimeError(
            f"toolchain source must be outside the target: {root}"
        )
    if destination.exists():
        shutil.rmtree(destination)

    gcc_bin = require_file(root / "bin" / f"{prefix}-gcc", f"{prefix}-gcc")
    toolchain_version = run([str(gcc_bin), "--version"],
                            capture=True).splitlines()[0]

    march = str(arch["march"])
    mabi = str(arch["mabi"])

    # Discover the libexec GCC version directory.
    libexec_base = root / "libexec" / "gcc" / prefix
    libexec_dirs = sorted(libexec_base.iterdir()) if libexec_base.is_dir() \
                   else []
    if not libexec_dirs:
        raise RuntimeError(f"no GCC libexec version found under {libexec_base}")
    gcc_ver = libexec_dirs[-1].name  # use highest version

    # Select the multilib directory.  Try exact match first; if the
    # toolchain has e.g. rv32imafdc_xtheade when we asked for
    # rv32imafc_xtheade, pick the closest rv32 superset.
    multilib_base = (root / "lib" / "gcc" / prefix / gcc_ver)
    multilib, _ = _find_best_multilib(multilib_base, march, mabi)

    # Select the target lib directory (riscv64-unknown-elf/lib/).
    target_lib_base = root / prefix / "lib"
    target_lib, _ = _find_best_multilib(target_lib_base, march, mabi)

    required_files = [
        f"bin/{prefix}-gcc",
        f"bin/{prefix}-g++",
        f"bin/{prefix}-ar",
        f"bin/{prefix}-as",
        f"bin/{prefix}-ld",
        f"bin/{prefix}-nm",
        f"bin/{prefix}-objcopy",
        f"bin/{prefix}-objdump",
        f"bin/{prefix}-ranlib",
        f"bin/{prefix}-readelf",
        f"bin/{prefix}-size",
        f"libexec/gcc/{prefix}/{gcc_ver}/cc1",
        f"libexec/gcc/{prefix}/{gcc_ver}/cc1plus",
        f"libexec/gcc/{prefix}/{gcc_ver}/collect2",
        f"libexec/gcc/{prefix}/{gcc_ver}/liblto_plugin.so.0.0.0",
        f"lib/gcc/{prefix}/{gcc_ver}/libgcc.a",
        f"lib/gcc/{prefix}/{gcc_ver}/crtbegin.o",
        f"lib/gcc/{prefix}/{gcc_ver}/crtend.o",
        f"lib/gcc/{prefix}/{gcc_ver}/crti.o",
        f"lib/gcc/{prefix}/{gcc_ver}/crtn.o",
        f"{prefix}/lib/nano.specs",
        f"{prefix}/lib/nosys.specs",
    ]
    for relative in required_files:
        copy_file(require_file(root / relative, f"toolchain {relative}"),
                  destination / relative,
                  executable=relative.startswith("bin/") or
                             relative.startswith("libexec/"))
    plugin = destination / f"libexec/gcc/{prefix}/{gcc_ver}/liblto_plugin.so"
    plugin.symlink_to("liblto_plugin.so.0.0.0")

    required_directories = [
        f"lib/gcc/{prefix}/{gcc_ver}/include",
        f"lib/gcc/{prefix}/{gcc_ver}/include-fixed",
        f"lib/gcc/{prefix}/{gcc_ver}/{multilib}",
        f"{prefix}/bin",
        f"{prefix}/include",
    ]
    if target_lib != ".":
        required_directories.append(f"{prefix}/lib/{target_lib}")

    for relative in required_directories:
        source = root / relative
        if not source.is_dir():
            raise RuntimeError(f"missing toolchain directory: {source}")
        shutil.copytree(source, destination / relative, symlinks=True)

    return toolchain_version


# ===================================================================
# Build orchestration
# ===================================================================


def record_source_versions(sdk: Path) -> dict[str, str]:
    """Record actual commits of SDK and its sub-repos.  Warn if dirty."""
    commit = run(["git", "-C", str(sdk), "rev-parse", "HEAD"], capture=True)
    source_commits = {"bouffalo_sdk": commit}
    sub_repos = [
        "drivers/lhal",
        "drivers/sys",
        "tools/bflb_tools",
    ]
    for relative in sub_repos:
        repo = sdk / relative
        if repo.is_dir():
            source_commits[relative] = run(
                ["git", "-C", str(repo), "rev-parse", "HEAD"], capture=True
            )

    # Also record soc/{chip}/std if it is a git repo
    soc_std = sdk / "drivers" / "soc"
    for child in soc_std.iterdir() if soc_std.is_dir() else []:
        child_git = child / "std" / ".git"
        if child_git.exists() or (child / "std").is_dir():
            try:
                source_commits[f"drivers/soc/{child.name}/std"] = run(
                    ["git", "-C", str(child / "std"),
                     "rev-parse", "HEAD"], capture=True
                )
            except subprocess.CalledProcessError:
                pass

    repos_to_check = [sdk] + [sdk / r for r in sub_repos]
    dirty = any(tracked_source_is_dirty(repo)
                for repo in repos_to_check
                if repo.is_dir())

    if dirty:
        print("WARNING: one or more source repositories have uncommitted "
              "changes.  The manifest will record '-dirty' and the bundle "
              "may not be reproducible.", file=sys.stderr)
        source_commits["bouffalo_sdk"] += "-dirty"

    return source_commits


def _install_host_tools(platform_root: Path, sdk: Path, chip: str) -> None:
    """Copy bflb_fw_post_proc and BLFlashCommand (with chip config) into platform."""
    tools_root = platform_root / "tools"

    post_proc = sdk / "tools" / "bflb_tools" / "bflb_fw_post_proc"
    pp_bin = require_file(post_proc / "bflb_fw_post_proc-ubuntu",
                          "Linux post processor")
    copy_file(pp_bin, tools_root / "bflb_fw_post_proc" / pp_bin.name,
              executable=True)

    flash_cube = sdk / "tools" / "bflb_tools" / "bouffalo_flash_cube"
    fc_bin = require_file(flash_cube / "BLFlashCommand-ubuntu",
                          "Linux FlashCube command")
    copy_file(fc_bin, tools_root / "bouffalo_flash_cube" / fc_bin.name,
              executable=True)
    fc_chip_dst = tools_root / "bouffalo_flash_cube" / "chips" / chip
    if fc_chip_dst.exists():
        shutil.rmtree(fc_chip_dst)
    fc_chip_src = flash_cube / "chips" / chip
    for relative in (
        Path("eflash_loader/eflash_loader_cfg.conf"),
        Path("efuse_bootheader/efuse_bootheader_cfg.conf"),
        Path("efuse_bootheader/flash_para.bin"),
    ):
        copy_file(require_file(fc_chip_src / relative,
                               f"FlashCube {relative}"),
                  fc_chip_dst / relative)


def _update_manifest_toolchain(sdk_runtime: Path, toolchain_version: str,
                               arch: dict[str, object], chip: str) -> None:
    """Update the toolchain field in an existing manifest.json in-place."""
    manifest_path = sdk_runtime / "manifest.json"
    if not manifest_path.is_file():
        return
    data = json.loads(manifest_path.read_text(encoding="utf-8"))
    data["toolchain"] = toolchain_version
    if "abi" in data:
        data["abi"].update({
            "march": arch["march"],
            "mabi": arch["mabi"],
            "mtune": arch["mtune"],
        })
    manifest_path.write_text(
        json.dumps(data, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--sdk", required=True, type=Path,
                        help="BouffaloSDK root directory")
    parser.add_argument("--chip", required=True,
                        choices=sorted(CHIP_CONFIG.keys()),
                        help="target chip (e.g. bl616cl)")
    parser.add_argument("--board", type=str, default=None,
                        help="board name (default: {chip}dk)")
    parser.add_argument("--toolchain", type=Path, default=None,
                        help="toolchain root (parent of bin/); "
                             "auto-detected from PATH if omitted")
    parser.add_argument("--tools-only", action="store_true",
                        help="skip SDK build — only update host tools "
                             "(bflb_fw_post_proc, BLFlashCommand) and toolchain")
    parser.add_argument("--keep-build", action="store_true",
                        help="retain the temporary CMake build directory")
    args = parser.parse_args()

    chip = args.chip
    chip_cfg = CHIP_CONFIG[chip]
    board = args.board or f"{chip}dk"
    toolchain_dirname = str(chip_cfg["toolchain_dirname"])
    prefix = str(chip_cfg["toolchain_prefix"])
    arch = chip_cfg["arch"]

    script_dir = Path(__file__).resolve().parent
    # platform root = hardware/bouffalo/bl616cl → tools/runtime_bundle is
    # two levels down
    platform_root = script_dir.parent.parent

    sdk = args.sdk.expanduser().resolve()
    toolchain_root = resolve_toolchain(args.toolchain, chip_cfg)

    # ——— validate SDK sources ————————————————————————————————
    if not (sdk / "project.build").is_file():
        raise RuntimeError(
            f"{sdk} does not look like a BouffaloSDK root "
            f"(missing project.build)"
        )

    board_dir = sdk / "bsp" / "board" / board
    if not board_dir.is_dir():
        raise RuntimeError(f"board directory not found: {board_dir}")

    soc_dir = sdk / "drivers" / "soc" / chip
    if not soc_dir.is_dir():
        raise RuntimeError(f"SOC directory not found: {soc_dir}")

    lhal_config_dir = sdk / "drivers" / "lhal" / "config" / chip
    if not lhal_config_dir.is_dir():
        raise RuntimeError(f"LHAL config directory not found: {lhal_config_dir}")

    version = sdk_version(sdk)
    source_commits = record_source_versions(sdk)
    commit = source_commits["bouffalo_sdk"]

    # ——— output paths ——————————————————————————————————————————
    sdk_runtime = platform_root / "tools" / "sdk" / chip
    tools_root = platform_root / "tools"

    print(f"Chip:               {chip}")
    print(f"Board:              {board}")
    print(f"SDK version:        {version}")
    print(f"SDK commit:         {commit}")
    print(f"Toolchain:          {toolchain_root}")
    print(f"Toolchain prefix:   {prefix}")
    print(f"Output SDK:         {sdk_runtime}")

    # ——— tools-only shortcut ——————————————————————————————————
    if args.tools_only:
        if not sdk_runtime.is_dir() or not (sdk_runtime / "manifest.json").is_file():
            raise RuntimeError(
                f"--tools-only requires an existing {sdk_runtime}. "
                f"Run without --tools-only first."
            )
        _install_host_tools(platform_root, sdk, chip)
        tc_dest = tools_root / toolchain_dirname
        toolchain_version = copy_minimal_toolchain(
            toolchain_root, tc_dest, prefix, arch
        )
        _update_manifest_toolchain(sdk_runtime, toolchain_version, arch, chip)
        print(f"\nToolchain installed in     {tc_dest}")
        print(f"Host tools updated from    {sdk}")
        return 0

    # ——— build the probe ———————————————————————————————————————
    build_dir = script_dir / "build"
    if build_dir.exists():
        shutil.rmtree(build_dir)

    env = os.environ.copy()
    env["BL_SDK_BASE"] = str(sdk)
    env["PATH"] = str(toolchain_root / "bin") + os.pathsep + \
                  env.get("PATH", "")

    make_args = [
        "make",
        f"CHIP={chip}",
        f"BOARD={board}",
        "CONFIG_PEC_V2=n",
        "CONFIG_MULTIMEDIA_VIDEO=n",
        f"BL_SDK_BASE={sdk}",
    ]
    run(make_args, cwd=script_dir, env=env)

    build_out = build_dir / "build_out"
    generated = build_dir / "generated"

    # ——— validate archives —————————————————————————————————————
    archives = {p.name: p for p in (build_out / "lib").glob("*.a")}
    expected = EXPECTED_SDK_ARCHIVES | {EXPECTED_BOARD_ARCHIVE}
    missing = expected - archives.keys()
    if missing:
        raise RuntimeError(
            f"build did not produce expected archives: {sorted(missing)}"
        )
    unexpected = archives.keys() - expected
    if unexpected:
        print(f"Note: additional SDK archives (ok): {sorted(unexpected)}")

    # ——— ELF sanity check ——————————————————————————————————————
    probe_elf_name = f"arduino_bl616cl_runtime_{chip}.elf"
    source_elf = require_file(build_out / probe_elf_name, "probe ELF")
    readelf = toolchain_root / "bin" / f"{prefix}-readelf"
    elf_header = run([str(readelf), "-h", str(source_elf)], capture=True)
    if "RISC-V" not in elf_header or \
       "Class:                             ELF32" not in elf_header:
        raise RuntimeError("probe ELF is not a 32-bit RISC-V image")

    # ——— validate side-car binaries —————————————————————————————
    side_cars = {p.name: p for p in build_out.glob("*.bin")}
    missing_cars = EXPECTED_SIDE_CARS - side_cars.keys()
    if missing_cars:
        raise RuntimeError(
            f"post processor did not produce: {sorted(missing_cars)}"
        )
    # boot2 naming varies: match any boot2*.bin
    boot2_bins = sorted(build_out.glob("boot2*.bin"))
    if not boot2_bins:
        raise RuntimeError("no boot2 binary found in build output")
    side_cars[boot2_bins[0].name] = boot2_bins[0]

    # ——— stage outputs atomically ———————————————————————————————
    partitions_root = platform_root / "tools" / "partitions"
    partitions_root.mkdir(parents=True, exist_ok=True)

    sdk_staging = sdk_runtime.with_name(f"{chip}.new")
    if sdk_staging.exists():
        shutil.rmtree(sdk_staging)

    (sdk_staging / "lib").mkdir(parents=True)
    (sdk_staging / "lib_board").mkdir(parents=True)
    (sdk_staging / "include").mkdir(parents=True)
    (sdk_staging / "include" / "board").mkdir(parents=True)
    (sdk_staging / "boot2").mkdir(parents=True)
    (sdk_staging / "dts").mkdir(parents=True)

    # SDK archives
    for name in sorted(EXPECTED_SDK_ARCHIVES):
        copy_file(archives[name], sdk_staging / "lib" / name)
    # board BSP archive → chip-level SDK (not variant)
    copy_file(archives[EXPECTED_BOARD_ARCHIVE],
              sdk_staging / "lib_board" / EXPECTED_BOARD_ARCHIVE)
    # autoconf.h and linker script
    copy_file(require_file(generated / "autoconf.h", "autoconf.h"),
              sdk_staging / "include" / "autoconf.h")
    copy_file(require_file(generated / "linker.ld",
                           f"{chip} linker script"), sdk_staging / "ld")
    # defconfig and FreeRTOSConfig.h from this directory
    copy_file(script_dir / "defconfig", sdk_staging / "defconfig")
    copy_file(script_dir / "FreeRTOSConfig.h",
              sdk_staging / "include" / "freertos" / "FreeRTOSConfig.h")

    # ——— SDK include roots ——————————————————————————————————————
    sdk_include_roots: list[tuple[Path, Path]] = [
        (sdk / "components" / "mm", Path("sdk/mm")),
        (sdk / "components" / "sysinit", Path("sdk/sysinit")),
        (sdk / "components" / "libc", Path("sdk/libc")),
        (sdk / "components" / "utils" / "log", Path("sdk/utils/log")),
        (sdk / "drivers" / "lhal" / "include", Path("sdk/lhal")),
        (lhal_config_dir, Path("sdk/lhal-config")),
        (sdk / "drivers" / "lhal" / "src" / "flash", Path("sdk/flash")),
        (sdk / "drivers" / "soc" / chip / "std" / "include",
         Path("sdk/soc")),
        (sdk / "drivers" / "sys", Path("sdk/sys")),
        (sdk / "components" / "os" / "freertos" / "include",
         Path("freertos")),
        (sdk / "components" / "os" / "freertos" / "portable" /
         "GCC" / "RISC-V" / "common", Path("freertos/portable")),
    ]
    # chip-specific FreeRTOS extension
    freertos_ext = chip_cfg.get("freertos_extension")
    if freertos_ext:
        ext_dir = (sdk / "components" / "os" / "freertos" /
                   "portable" / "GCC" / "RISC-V" / "common" /
                   "chip_specific_extensions" / str(freertos_ext))
        if ext_dir.is_dir():
            sdk_include_roots.append(
                (ext_dir, Path("freertos/chip_specific"))
            )

    copy_headers(sdk_include_roots, sdk_staging / "include")
    # board headers
    copy_headers([(board_dir, Path())], variant_staging / "include")
    # ring_buffer and other utils
    copy_headers(
        [(sdk / "components" / "utils" / "ring_buffer",
          Path("sdk/utils/ring_buffer")),
         (sdk / "components" / "utils" / "bflb_block_pool",
          Path("sdk/utils/bflb_block_pool")),
         (sdk / "components" / "utils" / "bflb_timestamp",
          Path("sdk/utils/bflb_timestamp")),
         (sdk / "components" / "utils" / "getopt",
          Path("sdk/utils/getopt")),
         (sdk / "components" / "utils" / "coredump",
          Path("sdk/utils/coredump")),
         (sdk / "components" / "utils" / "cjson",
          Path("sdk/utils/cjson")),
         (sdk / "components" / "utils" / "math" / "include",
          Path("sdk/utils/math/include")),
         (sdk / "components" / "utils" / "list",
          Path("sdk/utils/list")),
         ],
        sdk_staging / "include",
    )

    # ——— board headers → sdk ——————————————————————————————————
    copy_headers([(board_dir, Path("board"))], sdk_staging / "include")

    # ——— board config → sdk (DTS only) —————————————————————————
    board_cfg_dir = board_dir / "config"
    if board_cfg_dir.is_dir():
        for source in sorted(board_cfg_dir.iterdir()):
            if not source.is_file():
                continue
            if source.name.startswith("boot2") or source.name.startswith("partition"):
                continue
            # DTS → sdk_staging / dts
            copy_file(source, sdk_staging / "dts" / source.name)

    # ——— side-car binaries ——————————————————————————————————————
    # boot2 → chip-level sdk directory
    for b2 in boot2_bins:
        copy_file(b2, sdk_staging / "boot2" / b2.name)

    # efusedata side cars are compile-time outputs of bflb_fw_post_proc,
    # not checked-in assets.  The generator validates they are produced
    # but does not store them.

    # partition TOML → tools/partitions/ (platform-level)
    for source in sorted(board_cfg_dir.glob("partition*.toml")):
        copy_file(source, partitions_root / source.name)

    # variant pins_arduino.h is hand-maintained and NOT overwritten here.

    # ——— host tools —————————————————————————————————————————————
    _install_host_tools(platform_root, sdk, chip)

    # ——— toolchain ——————————————————————————————————————————————
    tc_dest = tools_root / toolchain_dirname
    toolchain_version = copy_minimal_toolchain(
        toolchain_root, tc_dest, prefix, arch
    )

    # ——— manifests ——————————————————————————————————————————————
    host_tool_paths = {
        "bflb_fw_post_proc": tools_root / "bflb_fw_post_proc" /
                             "bflb_fw_post_proc-ubuntu",
        "blflash": tools_root / "bouffalo_flash_cube" /
                   "BLFlashCommand-ubuntu",
        "gcc": tc_dest / "bin" / f"{prefix}-gcc",
    }
    host_tools = {
        name: {"sha256": sha256(path), "size": path.stat().st_size}
        for name, path in host_tool_paths.items()
    }

    def manifest_for(root: Path, scope: str) -> dict[str, object]:
        files = {}
        for path in sorted(root.rglob("*")):
            if path.is_file():
                files[str(path.relative_to(root))] = {
                    "sha256": sha256(path),
                    "size": path.stat().st_size,
                }
        return {
            "schema": 2,
            "scope": scope,
            "chip": chip,
            "board": board,
            "sdk_version": version,
            "source_commits": source_commits,
            "toolchain": toolchain_version,
            "host_tools": host_tools,
            "abi": {
                "march": arch["march"],
                "mabi": arch["mabi"],
                "mtune": arch["mtune"],
                "short_enums": True,
                "freertos": True,
                "cxx_standard": "gnu++17",
                "exceptions": False,
                "rtti": False,
            },
            "files": files,
        }

    (sdk_staging / "manifest.json").write_text(
        json.dumps(manifest_for(sdk_staging, "chip-runtime"),
                   indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )

    # ——— atomic swap ————————————————————————————————————————————
    if sdk_runtime.exists():
        shutil.rmtree(sdk_runtime)
    sdk_staging.rename(sdk_runtime)

    if not args.keep_build:
        shutil.rmtree(build_dir)

    print(f"\nChip runtime installed in  {sdk_runtime}")
    print(f"Toolchain installed in     {tc_dest}")
    print(f"SDK commit:                {commit}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
