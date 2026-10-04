#!/usr/bin/env python3
"""Build the Bouffalo SDK runtime for an Arduino compile from the SDK submodule.

This script is invoked by the platform's prebuild hook
(``recipe.hooks.prebuild.1.pattern`` in platform.txt) so that every Arduino
compile links headers and archives that were built from
``tools/sdk/bouffalo_sdk`` in this tree.  There is no checked-in binary
bundle: the SDK sources are the single source of truth and the runtime is
re-derived whenever the SDK revision, the probe configuration, the platform
patch set or the toolchain change.

The heavy SDK build is cached under the user cache directory (override with
``BOUFFALO_SDK_CACHE``).  A cache hit only re-materialises the staged runtime
inside the Arduino build directory (a symlink on POSIX systems).

Usage (normally driven by the platform hook):
    build_sdk_runtime.py --platform-root <platform> --chip bl616cl \
        --board bl616cldk --out <build.path>/sdk_runtime/bl616cl
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import generate_runtime_bundle as gen  # noqa: E402  (path set above)

RUNTIME_SCHEMA = 1

PROBE_FILES = (
    "Makefile",
    "main.c",
    "CMakeLists.txt",
    "defconfig",
    "FreeRTOSConfig.h",
    "usb_config.h",
)

# ---------------------------------------------------------------------------
# Small helpers
# ---------------------------------------------------------------------------


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def sha256_text(text: str) -> str:
    return hashlib.sha256(text.encode("utf-8", "surrogateescape")).hexdigest()


def git_output(repository: Path, *arguments: str) -> str | None:
    try:
        result = subprocess.run(
            ["git", "-C", str(repository), *arguments],
            text=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
        )
    except OSError:
        return None
    if result.returncode != 0:
        return None
    return result.stdout


def command_output(command: list[str]) -> str:
    result = subprocess.run(
        [str(part) for part in command], text=True, check=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    return result.stdout.strip()


def sdk_fingerprint(sdk: Path) -> dict[str, str]:
    """Identify the SDK source state with a cheap git fast path."""
    version = gen.sdk_version(sdk)
    top = git_output(sdk, "rev-parse", "--show-toplevel")
    if top is None or Path(top.strip()).resolve() != sdk.resolve():
        # Not a git worktree (board-manager install): fingerprint the tree.
        digest = hashlib.sha256()
        for path in sorted(sdk.rglob("*")):
            if not path.is_file():
                continue
            relative = path.relative_to(sdk)
            if any(part in ("build", ".git") for part in relative.parts):
                continue
            stat = path.stat()
            digest.update(f"{relative}:{stat.st_size}:{stat.st_mtime_ns}\n"
                          .encode("utf-8", "surrogateescape"))
        return {"version": version, "tree": digest.hexdigest()[:32]}

    head = git_output(sdk, "rev-parse", "HEAD")
    status = git_output(sdk, "status", "--porcelain=v1",
                        "--untracked-files=normal") or ""
    diff = git_output(sdk, "diff", "HEAD", "--binary") or ""
    fingerprint = {
        "version": version,
        "commit": (head or "").strip(),
        "status": sha256_text(status)[:16],
        "diff": sha256_text(diff)[:16],
    }
    # Untracked files are not covered by `git diff`; hash their content (small
    # files) or metadata (large ones) so new SDK drop-ins invalidate the cache.
    untracked = [line[3:] for line in status.splitlines()
                 if line.startswith("?? ")]
    if untracked:
        digest = hashlib.sha256()
        for name in sorted(untracked):
            path = sdk / name
            if path.is_file():
                stat = path.stat()
                if stat.st_size <= 4 * 1024 * 1024:
                    digest.update(f"{name}:{sha256_file(path)}".encode())
                else:
                    digest.update(f"{name}:{stat.st_size}:{stat.st_mtime_ns}"
                                  .encode())
        fingerprint["untracked"] = digest.hexdigest()[:16]
    return fingerprint


def default_cache_dir() -> Path:
    override = os.environ.get("BOUFFALO_SDK_CACHE")
    if override:
        return Path(override).expanduser()
    if sys.platform == "darwin":
        base = Path.home() / "Library" / "Caches"
    else:
        base = Path(os.environ.get("XDG_CACHE_HOME",
                                   Path.home() / ".cache"))
    return base / "arduino-bouffalo" / "sdk-runtime"


# ---------------------------------------------------------------------------
# Platform patch set
# ---------------------------------------------------------------------------


class PatchSet:
    """Apply the platform patch set to the SDK tree for the duration of a build."""

    def __init__(self, sdk: Path, patch_dir: Path) -> None:
        self.sdk = sdk
        self.patch_dir = patch_dir
        self.entries: list[dict[str, str]] = []
        manifest = patch_dir / "patches.json"
        if manifest.is_file():
            data = json.loads(manifest.read_text(encoding="utf-8"))
            for entry in data.get("patches", []):
                if entry.get("obsolete"):
                    continue
                self.entries.append({
                    "file": entry["file"],
                    "root": entry["root"],
                })

    def touched_files(self, entry: dict[str, str]) -> list[str]:
        files: list[str] = []
        for line in (self.patch_dir / entry["file"]).read_text(
                encoding="utf-8", errors="replace").splitlines():
            if line.startswith("+++ ") or line.startswith("--- "):
                name = line[4:].split("\t")[0]
                if name in ("/dev/null",):
                    continue
                if name.startswith(("a/", "b/")):
                    name = name[2:]
                files.append(name)
        return files

    def _patch(self, root: Path, patch: Path, *extra: str) -> subprocess.CompletedProcess:
        # --no-backup-if-mismatch: GNU patch otherwise drops ``<file>.orig``
        # backups next to SDK sources whenever the patch applies with an
        # offset/fuzz, leaving the submodule worktree dirty after a build.
        return subprocess.run(
            ["patch", "-p1", "--forward", "--no-backup-if-mismatch",
             *extra, "-i", str(patch)],
            cwd=root, text=True, stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
        )

    def _check(self, root: Path, patch: Path) -> bool:
        return self._patch(root, patch, "--dry-run").returncode == 0

    def _check_reverse(self, root: Path, patch: Path) -> bool:
        return self._patch(root, patch, "--dry-run", "--reverse").returncode == 0

    def verify_clean(self) -> None:
        """Refuse to patch files that already carry local modifications."""
        root = git_output(self.sdk, "rev-parse", "--show-toplevel")
        if root is None or Path(root.strip()).resolve() != self.sdk.resolve():
            return
        for entry in self.entries:
            files = self.touched_files(entry)
            if not files:
                continue
            status = git_output(self.sdk, "status", "--porcelain", "--",
                                *files) or ""
            if status.strip():
                raise RuntimeError(
                    f"SDK files touched by {entry['file']} have local "
                    f"modifications:\n{status}")

    def apply(self) -> None:
        self.verify_clean()
        for entry in self.entries:
            patch = self.patch_dir / entry["file"]
            root = self.sdk / entry["root"]
            if not root.is_dir():
                raise RuntimeError(f"patch root missing: {root}")
            if self._check(root, patch):
                result = self._patch(root, patch)
                if result.returncode != 0:
                    raise RuntimeError(
                        f"failed to apply {entry['file']}:\n{result.stdout}")
                print(f"Patched: {entry['file']} ({entry['root']})")
            elif self._check_reverse(root, patch):
                print(f"Note: patch already applied upstream: {entry['file']}")
            else:
                raise RuntimeError(
                    f"{entry['file']} does not apply to {root}; the patch set "
                    "and the pinned SDK revision are out of sync")

    def restore(self) -> None:
        for entry in reversed(self.entries):
            patch = self.patch_dir / entry["file"]
            root = self.sdk / entry["root"]
            if self._check_reverse(root, patch):
                self._patch(root, patch, "--reverse")
                print(f"Reverted: {entry['file']}")

    def residue(self) -> str:
        """Report modified patch-touched files, so drift cannot go unnoticed."""
        root = git_output(self.sdk, "rev-parse", "--show-toplevel")
        if root is None or Path(root.strip()).resolve() != self.sdk.resolve():
            return ""
        tracked: set[str] = set()
        for entry in self.entries:
            tracked.update((Path(entry["root"]) / name).as_posix()
                           for name in self.touched_files(entry))
        if not tracked:
            return ""
        return git_output(self.sdk, "status", "--porcelain", "--",
                          *sorted(tracked)) or ""


# ---------------------------------------------------------------------------
# Cache handling
# ---------------------------------------------------------------------------


def cache_key(*, platform_root: Path, sdk: Path, chip: str, board: str,
              extra_libs: dict[str, Path], probe_dir: Path) -> str:
    patch_dir = platform_root / "tools" / "runtime_bundle" / "patches"
    patch_material: dict[str, str] = {}
    manifest = patch_dir / "patches.json"
    if manifest.is_file():
        for entry in json.loads(manifest.read_text(encoding="utf-8")
                                ).get("patches", []):
            if entry.get("obsolete"):
                continue
            patch_material[entry["file"]] = sha256_file(
                patch_dir / entry["file"])
    material = {
        "schema": RUNTIME_SCHEMA,
        "builder": sha256_file(Path(__file__)),
        "generator": sha256_file(Path(gen.__file__)),
        "chip": chip,
        "board": board,
        "sdk": sdk_fingerprint(sdk),
        "defconfig": sha256_file(probe_dir / "defconfig"),
        "freertos_config": sha256_file(probe_dir / "FreeRTOSConfig.h"),
        "patches": patch_material,
        "extra_libs": {name: sha256_file(path)
                       for name, path in sorted(extra_libs.items())},
        "toolchain": command_output(
            [platform_root / "tools" / str(
                gen.CHIP_CONFIG[chip]["toolchain_dirname"]) / "bin" /
             (str(gen.CHIP_CONFIG[chip]["toolchain_prefix"]) + "-gcc"),
             "--version"]).splitlines()[0],
    }
    return sha256_text(json.dumps(material, sort_keys=True))[:16]


def materialize(entry: Path, out: Path) -> None:
    out.parent.mkdir(parents=True, exist_ok=True)
    if out.is_symlink():
        if out.resolve() == entry.resolve():
            return
        out.unlink()
    elif out.exists():
        shutil.rmtree(out)
    try:
        out.symlink_to(entry, target_is_directory=True)
    except OSError:
        shutil.copytree(entry, out)


def prune_cache(cache_root: Path, keep: Path) -> None:
    entries = [p for p in cache_root.iterdir()
               if p.is_dir() and p.name not in ("work",)]
    entries.sort(key=lambda p: p.stat().st_mtime, reverse=True)
    for stale in entries[3:]:
        if stale.resolve() == keep.resolve():
            continue
        shutil.rmtree(stale, ignore_errors=True)


# ---------------------------------------------------------------------------
# Probe / controller archive inputs
# ---------------------------------------------------------------------------


def resolve_extra_libs(*, platform_root: Path, sdk: Path, chip: str) -> dict[str, Path]:
    """Archives the SDK checkout does not ship for this target.

    SDK v2.3.36 and newer publish the BL616CL ``uarthci`` controller archive,
    so the normal result is empty and everything links out of the SDK tree.
    A controller flavor the SDK does not ship can still be brought up with
    ``BOUFFALO_BLE_CONTROLLER_LIB=<archive>``; the builder stages it into the
    SDK tree for the link and removes it again.
    """
    variant = gen.defconfig_value(
        platform_root / "tools" / "runtime_bundle" / "defconfig",
        "CONFIG_BTBLECONTROLLER_LIB")
    if not variant:
        return {}
    name = f"libbtblecontroller_{chip}_{variant}.a"
    sdk_copy = (sdk / "components" / "wireless" / "bluetooth" /
                "btblecontroller" / "lib" / name)
    if sdk_copy.is_file():
        return {}
    override = os.environ.get("BOUFFALO_BLE_CONTROLLER_LIB")
    if override:
        path = Path(override)
        if path.is_file():
            return {name: path}
        raise RuntimeError(
            f"BOUFFALO_BLE_CONTROLLER_LIB does not point at a file: {path}")
    raise RuntimeError(
        f"{name} is not shipped by {sdk}; update the SDK submodule or set "
        f"BOUFFALO_BLE_CONTROLLER_LIB to a prebuilt archive")


def write_probe_dir(platform_root: Path, probe_dir: Path) -> None:
    template_dir = platform_root / "tools" / "runtime_bundle"
    probe_dir.mkdir(parents=True, exist_ok=True)
    for name in PROBE_FILES:
        shutil.copy2(template_dir / name, probe_dir / name)


def make_work_dir(probe_dir: Path, work_dir: Path) -> None:
    if work_dir.exists():
        shutil.rmtree(work_dir)
    shutil.copytree(probe_dir, work_dir)


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--platform-root", type=Path, required=True)
    parser.add_argument("--chip", required=True,
                        choices=sorted(gen.CHIP_CONFIG))
    parser.add_argument("--board", required=True)
    parser.add_argument("--out", type=Path, required=True,
                        help="runtime directory consumed by platform.txt")
    parser.add_argument("--cache-dir", type=Path,
                        default=default_cache_dir())
    parser.add_argument("--force", action="store_true",
                        help="rebuild even when the cache entry exists")
    parser.add_argument("--no-patches", action="store_true",
                        help="do not apply the platform patch set")
    parser.add_argument("--keep-work", action="store_true",
                        help="keep the SDK build directory after staging")
    args = parser.parse_args()

    platform_root = args.platform_root.expanduser().resolve()
    chip = args.chip
    chip_cfg = gen.CHIP_CONFIG[chip]
    prefix = str(chip_cfg["toolchain_prefix"])
    board = args.board
    cache_root = args.cache_dir.expanduser().resolve()
    cache_root.mkdir(parents=True, exist_ok=True)

    sdk = platform_root / "tools" / "sdk" / "bouffalo_sdk"
    if not (sdk / "project.build").is_file():
        raise RuntimeError(
            f"{sdk} is not a BouffaloSDK checkout; run "
            "'git submodule update --init tools/sdk/bouffalo_sdk'")
    if not (sdk / "bsp" / "board" / board).is_dir():
        raise RuntimeError(f"SDK has no board directory for {board}")

    toolchain_root = platform_root / "tools" / str(chip_cfg["toolchain_dirname"])
    if not (toolchain_root / "bin" / f"{prefix}-gcc").is_file():
        raise RuntimeError(f"toolchain not found: {toolchain_root}")

    probe_dir = cache_root / "probe"
    write_probe_dir(platform_root, probe_dir)
    extra_libs = resolve_extra_libs(platform_root=platform_root, sdk=sdk,
                                    chip=chip)

    key_start = time.time()
    key = cache_key(platform_root=platform_root, sdk=sdk, chip=chip,
                    board=board, extra_libs=extra_libs, probe_dir=probe_dir)
    entry = cache_root / key
    print(f"SDK runtime: chip={chip} board={board} key={key} "
          f"({time.time() - key_start:.1f}s)")

    if entry.is_dir() and not args.force and (entry / "manifest.json").is_file():
        print(f"Cache hit: {entry}")
        materialize(entry, args.out)
        prune_cache(cache_root, entry)
        return 0

    staging = cache_root / f"{key}.new"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir(parents=True)

    work_dir = cache_root / "work" / key
    make_work_dir(probe_dir, work_dir)

    patches = PatchSet(sdk, platform_root / "tools" / "runtime_bundle" / "patches")
    toolchain_version = command_output(
        [toolchain_root / "bin" / f"{prefix}-gcc", "--version"]).splitlines()[0]

    try:
        if args.no_patches:
            print("Patch set disabled (--no-patches)")
        else:
            patches.apply()
        patch_hashes = {
            entry["file"]: sha256_file(Path(patches.patch_dir) / entry["file"])
            for entry in patches.entries
        }
        probe = gen.build_sdk_probe(
            sdk=sdk, chip=chip, board=board, chip_cfg=chip_cfg,
            work_dir=work_dir, toolchain_root=toolchain_root, prefix=prefix,
            extra_libs=extra_libs,
        )
        # Staging reads headers and archives out of the probe work tree and
        # must therefore run before the work tree is removed, while the patch
        # set is still applied (patched SDK headers are part of the runtime).
        gen.stage_sdk_runtime(
            sdk=sdk, chip=chip, chip_cfg=chip_cfg, board=board,
            template_dir=probe_dir, destination=staging, probe=probe,
            extra_libs=extra_libs,
            version=gen.sdk_version(sdk),
            source_commits=gen.record_source_versions(sdk),
            toolchain_version=toolchain_version,
            manifest_scope="build-runtime",
            manifest_extra={
                "cache_key": key,
                "patches": patch_hashes,
                "extra_libs": {name: sha256_file(path)
                               for name, path in sorted(extra_libs.items())},
            },
        )
        if entry.exists():
            shutil.rmtree(entry, ignore_errors=True)
        staging.rename(entry)
        materialize(entry, args.out)
        prune_cache(cache_root, entry)
    except BaseException:
        shutil.rmtree(staging, ignore_errors=True)
        raise
    finally:
        if not args.no_patches:
            patches.restore()
            leftover = patches.residue()
            if leftover:
                print("warning: SDK tree is dirty after patch restore:\n"
                      f"{leftover}", file=sys.stderr)
        if not args.keep_work:
            shutil.rmtree(work_dir, ignore_errors=True)

    print(f"SDK runtime ready: {args.out} -> {entry}")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
