#!/usr/bin/env python3
"""Build Bouffalo Arduino SDK/variant bundles from a BouffaloSDK checkout.

The Arduino platform no longer consumes a checked-in bundle: the supported
flow is the compile-time runtime built by build_sdk_runtime.py, which imports
the shared helpers from this file.  This CLI still generates a full bundle
into an explicit --out path (used in the past for the removed checked-in
bundles) and installs the host toolchain subset with --tools-only:
  <out>/                     chip-level headers, archives, linker script, ...
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

# Archives every chip build MUST produce (subset of actual output).
CORE_SDK_ARCHIVES = {
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


def git_head(repository: Path) -> str | None:
    """Return HEAD of *repository* when it is a usable git worktree root.

    Release SDK snapshots may ship dangling ``.git`` gitdir files that point
    at a missing ``.git/modules`` directory (the submodule metadata is not part
    of the release tarball).  Such directories are not usable repositories;
    return None instead of aborting the bundle build.
    """
    try:
        result = subprocess.run(
            ["git", "-C", str(repository), "rev-parse",
             "--show-toplevel", "HEAD"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None
    parts = result.stdout.split()
    if len(parts) != 2:
        return None
    toplevel, commit = parts
    try:
        if Path(toplevel).resolve() != repository.resolve():
            return None
    except OSError:
        return None
    return commit


def tracked_source_is_dirty(repository: Path) -> bool:
    try:
        result = subprocess.run(
            ["git", "-C", str(repository),
             "status", "--porcelain=v1", "--untracked-files=no"],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        )
    except OSError:
        return False
    if result.returncode != 0:
        return False
    return bool(result.stdout.strip())


def first_existing(*candidates: Path) -> Path | None:
    """Return the first candidate that exists as a file, else None."""
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    return None


def defconfig_value(path: Path, key: str) -> str | None:
    """Return the value assigned to *key* in a defconfig/.config file."""
    if not path.is_file():
        return None
    pattern = re.compile(rf"^{re.escape(key)}\s*=\s*(\S+)\s*$")
    for line in path.read_text(encoding="utf-8").splitlines():
        match = pattern.match(line)
        if match:
            return match.group(1)
    return None


def copy_headers(source_roots: list[tuple[Path, Path]],
                 include_root: Path) -> None:
    for source_root, relative_root in source_roots:
        if not source_root.is_dir():
            raise RuntimeError(f"missing include directory: {source_root}")
        for source in sorted(source_root.rglob("*.h")):
            destination = (include_root / relative_root /
                           source.relative_to(source_root))
            copy_file(source, destination)


def copy_cherryusb_headers(sdk: Path, include_root: Path,
                           runtime_bundle: Path) -> None:
    """Copy CherryUSB's flat include surface into the SDK runtime include dir.

    CherryUSB headers deliberately use short, unqualified include names such as
    ``usbd_core.h`` and ``usbd_hid.h``.  Flattening them into ``include/`` keeps
    the Arduino platform recipes small and matches the upstream SDK examples.
    """
    cherryusb_root = sdk / "components" / "usb" / "cherryusb"
    if not cherryusb_root.is_dir():
        raise RuntimeError(f"missing CherryUSB source tree: {cherryusb_root}")

    for source in sorted(cherryusb_root.rglob("*.h")):
        copy_file(source, include_root / source.name)

    copy_file(
        require_file(runtime_bundle / "usb_config.h", "CherryUSB config"),
        include_root / "usb_config.h",
    )


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
        f"libexec/gcc/{prefix}/{gcc_ver}/lto-wrapper",
        f"libexec/gcc/{prefix}/{gcc_ver}/lto1",
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
    """Record actual commits of SDK and its sub-repos.  Warn if dirty.

    Sub-repositories are recorded only when they are real git worktrees.
    Release snapshots may ship dangling ``.git`` gitdir files (see
    :func:`git_head`); those are skipped rather than aborting the build.
    """
    commit = git_head(sdk)
    if commit is None:
        raise RuntimeError(f"{sdk} is not a usable git worktree")
    source_commits = {"bouffalo_sdk": commit}
    sub_repos = [
        "drivers/lhal",
        "drivers/sys",
        "tools/bflb_tools",
    ]
    for relative in sub_repos:
        repo = sdk / relative
        if repo.is_dir():
            sub_commit = git_head(repo)
            if sub_commit is not None:
                source_commits[relative] = sub_commit

    # Also record soc/{chip}/std if it is a git repo
    soc_std = sdk / "drivers" / "soc"
    if soc_std.is_dir():
        for child in sorted(soc_std.iterdir()):
            if not child.is_dir():
                continue
            sub_commit = git_head(child / "std")
            if sub_commit is not None:
                source_commits[f"drivers/soc/{child.name}/std"] = sub_commit

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


# ---------------------------------------------------------------------------
# Shared probe build and runtime staging
# ---------------------------------------------------------------------------


def sdk_include_roots(sdk: Path, chip: str, chip_cfg: dict[str, object],
                      lhal_config_dir: Path,
                      mbedtls_v2: bool) -> list[tuple[Path, Path]]:
    """Return the (source directory, staged relative path) include map.

    The Arduino recipes consume a flat runtime layout rooted at
    ``compiler.sdk.path``.  Keeping the map here means a new chip only needs a
    CHIP_CONFIG entry: its headers are re-derived from the SDK sources instead
    of being copied into the platform tree by hand.
    """
    mbedtls_root = sdk / "components" / "crypto" / "mbedtls"
    roots: list[tuple[Path, Path]] = [
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
        # Wireless / net stack headers referenced by the platform.txt -I
        # flags (<lwip/...>, <mbedtls/...>, fhost, supplicant, macsw,
        # phyrf, rfparam).  Without these the Arduino compile breaks on
        # lwip/inet.h and friends.
        (sdk / "components" / "net" / "lwip" / "lwip" / "src" / "include",
         Path("sdk/lwip")),
        (sdk / "components" / "net" / "lwip" / "lwip" / "src" / "include" /
         "compat" / "posix", Path("sdk/lwip-posix")),
        (sdk / "components" / "net" / "lwip" / "lwip" / "lwip-port",
         Path("sdk/lwip-port")),
        (sdk / "components" / "wireless" / "wifi6" / "fhost" / "include",
         Path("sdk/wifi6")),
        (sdk / "components" / "wireless" / "bl_wpa_supplicant" / "include",
         Path("sdk/supplicant")),
        (sdk / "components" / "wireless" / "macsw" / "inc",
         Path("sdk/macsw")),
        (mbedtls_root / ("mbedtls" if mbedtls_v2 else "mbedtls_v3") /
         "include", Path("sdk/mbedtls")),
        # mbedtls hardware-acceleration port headers (ecp_alt.h & friends are
        # included by name from the public mbedtls/*.h headers)
        (mbedtls_root / "port" / "hw_acc", Path("sdk/mbedtls")),
        (mbedtls_root / "port", Path("sdk/mbedtls/port")),
        (sdk / "drivers" / "soc" / chip / "phyrf" / "include",
         Path("sdk/phyrf")),
        (sdk / "drivers" / "rfparam" / "Inc", Path("sdk/rfparam")),
        # headers included by bare name via the existing -Iinclude/sdk/utils
        (sdk / "components" / "utils" / "async_event", Path("sdk/utils")),
        (sdk / "components" / "utils" / "partition", Path("sdk/utils")),
    ]
    if not mbedtls_v2:
        roots.append((mbedtls_root / "mbedtls_v3" / "3rdparty" / "everest" /
                      "include", Path("sdk/mbedtls")))
    freertos_ext = chip_cfg.get("freertos_extension")
    if freertos_ext:
        ext_dir = (sdk / "components" / "os" / "freertos" /
                   "portable" / "GCC" / "RISC-V" / "common" /
                   "chip_specific_extensions" / str(freertos_ext))
        if ext_dir.is_dir():
            roots.append((ext_dir, Path("freertos/chip_specific")))
    return roots


def build_sdk_probe(*, sdk: Path, chip: str, board: str,
                    chip_cfg: dict[str, object], work_dir: Path,
                    toolchain_root: Path, prefix: str,
                    extra_libs: dict[str, Path] | None = None) -> dict[str, object]:
    """Compile the SDK probe application inside *work_dir*.

    *work_dir* must contain the probe template files (Makefile, main.c,
    CMakeLists.txt, defconfig, FreeRTOSConfig.h).  Archives named in
    *extra_libs* that the SDK does not ship are staged into the SDK tree for
    the duration of the link and removed afterwards.
    """
    extra_libs = dict(extra_libs or {})
    build_dir = work_dir / "build"

    env = os.environ.copy()
    env["BL_SDK_BASE"] = str(sdk)
    # The SDK's Kconfig/CMake helpers import Python modules whose bytecode
    # caches are tracked in the submodule; do not rewrite them during a build.
    env["PYTHONDONTWRITEBYTECODE"] = "1"
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

    # The SDK Kconfig registers
    # btblecontroller/lib/libbtblecontroller_{chip}_{variant}.a unconditionally
    # in the link, but some released variants (BL616CL uarthci) are not
    # published in the SDK tree.  Stage the platform-provided archive into the
    # SDK for the duration of the link and remove it afterwards so the SDK
    # checkout is left untouched.
    controller_variant = defconfig_value(work_dir / "defconfig",
                                         "CONFIG_BTBLECONTROLLER_LIB")
    staged_controller: Path | None = None
    if controller_variant:
        controller_name = f"libbtblecontroller_{chip}_{controller_variant}.a"
        controller_path = (sdk / "components" / "wireless" / "bluetooth" /
                           "btblecontroller" / "lib" / controller_name)
        if not controller_path.is_file() and controller_name in extra_libs:
            controller_path.parent.mkdir(parents=True, exist_ok=True)
            copy_file(extra_libs[controller_name], controller_path)
            staged_controller = controller_path
            print("Note: staged controller archive for link: "
                  f"{controller_name}")
    try:
        run(make_args, cwd=work_dir, env=env)
    finally:
        if staged_controller is not None and staged_controller.is_file():
            staged_controller.unlink()

    build_out = build_dir / "build_out"
    generated = build_dir / "generated"

    # ——— validate archives —————————————————————————————————————
    archives = {p.name: p for p in (build_out / "lib").glob("*.a")}
    required = CORE_SDK_ARCHIVES | {EXPECTED_BOARD_ARCHIVE}
    missing = required - archives.keys()
    if missing:
        raise RuntimeError(
            f"build did not produce required archives: {sorted(missing)}"
        )
    extras = archives.keys() - required
    if extras:
        print(f"Note: additional SDK archives (ok): {sorted(extras)}")

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

    return {
        "build_dir": build_dir,
        "build_out": build_out,
        "generated": generated,
        "archives": archives,
        "boot2_bins": boot2_bins,
    }


def stage_sdk_runtime(*, sdk: Path, chip: str, chip_cfg: dict[str, object],
                      board: str, template_dir: Path, destination: Path,
                      probe: dict[str, object],
                      extra_libs: dict[str, Path] | None = None,
                      preserve_from: Path | None = None,
                      preserve_libs: tuple[str, ...] = (),
                      version: str, source_commits: dict[str, str],
                      toolchain_version: str,
                      manifest_scope: str = "chip-runtime",
                      manifest_extra: dict[str, object] | None = None,
                      ) -> dict[str, object]:
    """Lay out a buildable Arduino runtime under *destination*.

    ``destination`` must exist and be empty; the caller performs the atomic
    swap/rename once this returns.
    """
    extra_libs = dict(extra_libs or {})
    probe = dict(probe)
    build_dir = Path(probe["build_dir"])
    build_out = Path(probe["build_out"])
    generated = Path(probe["generated"])
    archives: dict[str, Path] = probe["archives"]  # type: ignore[assignment]
    boot2_bins: list[Path] = probe["boot2_bins"]  # type: ignore[assignment]

    board_dir = sdk / "bsp" / "board" / board

    (destination / "lib").mkdir(parents=True)
    (destination / "lib_board").mkdir(parents=True)
    (destination / "include").mkdir(parents=True)
    (destination / "include" / "board").mkdir(parents=True)
    (destination / "boot2").mkdir(parents=True)
    (destination / "dts").mkdir(parents=True)

    # SDK archives — copy ALL .a files (core + optional like BLE/WiFi)
    for name in sorted(archives):
        if name == EXPECTED_BOARD_ARCHIVE:
            continue
        copy_file(archives[name], destination / "lib" / name)

    # btblecontroller — precompiled BLE controller archive.  Development
    # checkouts build it from source under build_btblecontroller/; release
    # snapshots ship it under components/wireless/bluetooth/btblecontroller/
    # lib/.  Flavors the SDK does not ship (the uarthci archive before SDK
    # v2.3.36) arrive through extra_libs.  platform.txt links it by name.
    controller_name: str | None = None
    controller_variant = defconfig_value(template_dir / "defconfig",
                                         "CONFIG_BTBLECONTROLLER_LIB")
    if controller_variant:
        controller_name = f"libbtblecontroller_{chip}_{controller_variant}.a"
        controller_source = first_existing(
            build_dir / "build_btblecontroller" / controller_name,
            sdk / "components" / "wireless" / "bluetooth" /
            "btblecontroller" / "lib" / controller_name,
        )
        if controller_source is None:
            controller_source = extra_libs.get(controller_name)
        if controller_source is not None:
            copy_file(controller_source, destination / "lib" / controller_name)
            print(f"Note: copied btblecontroller: {controller_name}")
        else:
            print(f"WARNING: BLE controller archive not found, "
                  f"skipping: {controller_name}", file=sys.stderr)

    # Chip-specific archives live in component directories rather than
    # build_out.  Development checkouts build them from source under
    # build_wl80211/ or build_macsw/; release snapshots ship the same archives
    # precompiled under components/wireless/*/lib/.  platform.txt links them by
    # these exact names, so accept either layout.
    wireless_root = sdk / "components" / "wireless"
    macsw_flavor = (defconfig_value(template_dir / "defconfig",
                                    "CONFIG_MACSW_SELECT") or "default")
    extra_archives: dict[str, Path | None] = {
        f"libpka_{chip}.a": first_existing(
            sdk / "drivers" / "lhal" / "src" / "pka" / f"libpka_{chip}.a",
        ),
        f"libwl80211_{chip}.a": first_existing(
            build_dir / "build_wl80211" / "src" / f"libwl80211_{chip}.a",
            wireless_root / "wl80211" / "lib" / f"libwl80211_{chip}.a",
        ),
        f"libmacsw_{chip}.a": first_existing(
            build_dir / "build_macsw" / f"libmacsw_{chip}.a",
            wireless_root / "macsw" / "lib" / f"libmacsw_{chip}.a",
        ),
        f"libmacsw_config_{chip}_{macsw_flavor}.a": first_existing(
            build_dir / "build_macsw" /
            f"libmacsw_config_{chip}_{macsw_flavor}.a",
            wireless_root / "macsw" / "lib" /
            f"libmacsw_config_{chip}_{macsw_flavor}.a",
        ),
    }
    for name, source in extra_archives.items():
        if source is not None:
            copy_file(source, destination / "lib" / name)
        else:
            print(f"WARNING: chip archive not found, skipping: {name}",
                  file=sys.stderr)

    # Platform-provided archives that the SDK checkout does not ship.
    for name, source in sorted(extra_libs.items()):
        if (name in extra_archives or name in archives or
                name == controller_name):
            continue
        copy_file(source, destination / "lib" / name)

    # Legacy carry-over path (release bundle regeneration keeps archives that
    # are not reproducible from the SDK sources).
    if preserve_from is not None:
        for preserved in preserve_libs:
            existing = preserve_from / "lib" / preserved
            if existing.is_file():
                copy_file(existing, destination / "lib" / preserved)

    # phyrf — precompiled RF calibration library (required by BLE/WiFi)
    phyrf_dir = sdk / "drivers" / "soc" / chip / "phyrf"
    if phyrf_dir.is_dir():
        for phyrf_candidate in sorted(phyrf_dir.glob("lib-*/lib*_phyrf.a")):
            copy_file(phyrf_candidate, destination / "lib" / phyrf_candidate.name)
            print(f"Note: copied phyrf: {phyrf_candidate.name}")

    # board BSP archive → chip-level SDK (not variant)
    copy_file(archives[EXPECTED_BOARD_ARCHIVE],
              destination / "lib_board" / EXPECTED_BOARD_ARCHIVE)
    # autoconf.h and linker script
    generated_autoconf = generated / "autoconf.h"
    if not generated_autoconf.is_file():
        generated_autoconf = generated / "autoconfig.h"
    copy_file(require_file(generated_autoconf, "autoconf.h"),
              destination / "include" / "autoconf.h")
    copy_file(require_file(generated / "linker.ld",
                           f"{chip} linker script"), destination / "ld")
    # The linker script includes the MACSW cache-affinity fragment by name.
    # Keep that fragment beside the generated script in the runtime.
    macsw_affinity = (sdk / "components" / "wireless" / "macsw" /
                      "macsw_cache_affinity.ld.in")
    if macsw_affinity.is_file():
        copy_file(macsw_affinity, destination / macsw_affinity.name)
    # defconfig and FreeRTOSConfig.h from the probe template directory
    copy_file(template_dir / "defconfig", destination / "defconfig")
    copy_file(template_dir / "FreeRTOSConfig.h",
              destination / "include" / "freertos" / "FreeRTOSConfig.h")

    # ——— SDK include roots ——————————————————————————————————————
    lhal_config_dir = sdk / "drivers" / "lhal" / "config" / chip
    include_roots = sdk_include_roots(
        sdk, chip, chip_cfg, lhal_config_dir,
        defconfig_value(template_dir / "defconfig",
                        "CONFIG_MBEDTLS_V2") == "y")

    copy_headers(include_roots, destination / "include")
    copy_cherryusb_headers(sdk, destination / "include", template_dir)
    # mbedtls config header picked up via MBEDTLS_CONFIG_FILE
    if defconfig_value(template_dir / "defconfig",
                       "CONFIG_MBEDTLS_V2") == "y":
        copy_file(require_file(template_dir / "mbedtls_sample_config.h",
                               "mbedTLS v2 sample config"),
                  destination / "include" / "sdk" / "mbedtls" /
                  "mbedtls_sample_config.h")
    else:
        for cfg_name in ("config-tls-generic.h", "config-psa.h"):
            copy_file(require_file(sdk / "components" / "crypto" / "mbedtls" /
                                   cfg_name, cfg_name),
                      destination / "include" / "sdk" / "mbedtls" / cfg_name)
    # generated Kconfig autoconf (platform recipe does -include autoconf.h)
    autoconf = None
    for candidate in (build_out / "include" / "autoconf.h",
                      build_dir / "generated" / "include" / "autoconf.h",
                      build_dir / "generated" / "autoconf.h",
                      build_dir / "generated" / "autoconfig.h"):
        if candidate.is_file():
            autoconf = candidate
            break
    if autoconf is None:
        raise RuntimeError("generated autoconf.h not found in build output")
    copy_file(autoconf, destination / "include" / "autoconf.h")
    # board headers → sdk/include/board
    copy_headers([(board_dir, Path("board"))], destination / "include")
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
        destination / "include",
    )

    # ——— board config → sdk (DTS only) —————————————————————————
    board_cfg_dir = board_dir / "config"
    if board_cfg_dir.is_dir():
        for source in sorted(board_cfg_dir.iterdir()):
            if not source.is_file():
                continue
            if source.name.startswith("boot2") or \
               source.name.startswith("partition"):
                continue
            copy_file(source, destination / "dts" / source.name)

    # ——— side-car binaries ——————————————————————————————————————
    for boot2 in boot2_bins:
        copy_file(boot2, destination / "boot2" / boot2.name)

    # ——— manifest ————————————————————————————————————————————
    files: dict[str, dict[str, object]] = {}
    for path in sorted(destination.rglob("*")):
        if path.is_file():
            files[str(path.relative_to(destination))] = {
                "sha256": sha256(path),
                "size": path.stat().st_size,
            }
    manifest: dict[str, object] = {
        "schema": 2,
        "scope": manifest_scope,
        "chip": chip,
        "board": board,
        "sdk_version": version,
        "source_commits": source_commits,
        "toolchain": toolchain_version,
    }
    if manifest_extra:
        manifest.update(manifest_extra)
    manifest["files"] = files
    (destination / "manifest.json").write_text(
        json.dumps(manifest, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
    )
    return manifest


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

    # Archives the SDK checkout does not ship for this target are carried over
    # from the existing runtime during release-time regeneration.  The
    # compile-time builder (build_sdk_runtime.py) takes them from the SDK tree
    # and the optional BOUFFALO_BLE_CONTROLLER_LIB override instead.
    extra_libs: dict[str, Path] = {}
    for carried_name in ("libbtblecontroller_bl616cl_uarthci.a",
                         "libblestack.a"):
        carried = sdk_runtime / "lib" / carried_name
        if carried.is_file():
            extra_libs[carried_name] = carried

    probe = build_sdk_probe(
        sdk=sdk, chip=chip, board=board, chip_cfg=chip_cfg,
        work_dir=script_dir, toolchain_root=toolchain_root, prefix=prefix,
        extra_libs=extra_libs,
    )
    build_dir = Path(probe["build_dir"])

    # ——— toolchain ——————————————————————————————————————————————
    tc_dest = tools_root / toolchain_dirname
    toolchain_version = copy_minimal_toolchain(
        toolchain_root, tc_dest, prefix, arch
    )

    # ——— partitions ————————————————————————————————————————————
    partitions_root = platform_root / "tools" / "partitions"
    partitions_root.mkdir(parents=True, exist_ok=True)
    board_cfg_dir = sdk / "bsp" / "board" / board / "config"
    if board_cfg_dir.is_dir():
        for source in sorted(board_cfg_dir.glob("partition*.toml")):
            copy_file(source, partitions_root / source.name)

    # ——— host tools —————————————————————————————————————————————
    _install_host_tools(platform_root, sdk, chip)
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

    # ——— stage outputs atomically ———————————————————————————————
    sdk_staging = sdk_runtime.with_name(f"{chip}.new")
    if sdk_staging.exists():
        shutil.rmtree(sdk_staging)
    sdk_staging.mkdir(parents=True)

    stage_sdk_runtime(
        sdk=sdk, chip=chip, chip_cfg=chip_cfg, board=board,
        template_dir=script_dir, destination=sdk_staging, probe=probe,
        extra_libs=extra_libs,
        version=version, source_commits=source_commits,
        toolchain_version=toolchain_version,
        manifest_extra={
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
        },
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
