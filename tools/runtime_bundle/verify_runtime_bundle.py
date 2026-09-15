#!/usr/bin/env python3
"""Verify a BL616CL runtime bundle is internally consistent and reproducible."""

from __future__ import annotations

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

from verify_source_manifests import verify_dependency_manifests


REQUIRED_LIBRARIES = {
    "libfhost_bl616cl_default.a",
    "libfhost_config.a",
    "liblwip.a",
    "libmacsw_bl616cl.a",
    "libmacsw_config_bl616cl_default.a",
    "libmacsw_os_adapter.a",
    "libmacsw_plat.a",
    "libwifi6_lwip_adapter.a",
    "libwpa_supplicant.a",
    "libbtblecontroller_bl616cl_m2s1.a",
    "libblestack.a",
}

REQUIRED_HEADERS = {
    "include/wifi/fhost/fhost_api.h",
    "include/wifi/fhost/wifi_mgmr.h",
    "include/wifi/fhost/wifi_mgmr_ext.h",
    "include/wifi/macsw/macsw.h",
}


def project_root(script_dir: Path) -> Path:
    """Return the root repository containing third_party/bouffalo_sdk."""
    return script_dir.parents[1]


def controlled_sdk_path(script_dir: Path) -> Path:
    return project_root(script_dir) / "third_party" / "bouffalo_sdk"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def git_value(repository: Path, *arguments: str) -> str:
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments],
        check=False,
        capture_output=True,
        text=True,
    )
    if result.returncode != 0:
        raise RuntimeError(f"git query failed for {repository}: {' '.join(arguments)}")
    return result.stdout.strip()


def repository_is_dirty(repository: Path) -> bool:
    return bool(
        git_value(
            repository,
            "status",
            "--porcelain=v1",
            "--untracked-files=no",
        )
    )


def verify_manifest_files(bundle: Path, manifest: dict[str, object]) -> None:
    files = manifest.get("files")
    if not isinstance(files, dict):
        raise RuntimeError("manifest files field is missing or invalid")

    expected_files = set(files)
    actual_files = {
        str(path.relative_to(bundle))
        for path in bundle.rglob("*")
        if path.is_file() and path.name != "manifest.json"
    }
    unmanaged_files = sorted(actual_files - expected_files)
    if unmanaged_files:
        raise RuntimeError(
            "bundle contains files omitted from manifest: "
            + ", ".join(unmanaged_files)
        )
    missing_records = sorted(expected_files - actual_files)
    if missing_records:
        raise RuntimeError(
            "manifest records files absent from bundle: "
            + ", ".join(missing_records)
        )

    for relative, metadata in files.items():
        path = bundle / relative
        if not path.is_file():
            raise RuntimeError(f"manifest file is missing: {relative}")
        if not isinstance(metadata, dict):
            raise RuntimeError(f"manifest metadata is invalid: {relative}")
        expected_hash = metadata.get("sha256")
        expected_size = metadata.get("size")
        if expected_hash != sha256(path):
            raise RuntimeError(f"manifest SHA-256 mismatch: {relative}")
        if expected_size != path.stat().st_size:
            raise RuntimeError(f"manifest size mismatch: {relative}")


def verify_sdk_commits(
    sdk: Path,
    source_commits: dict[str, object],
    allow_dirty: bool,
) -> None:
    if not (sdk / "project.build").is_file():
        raise RuntimeError(f"not a Bouffalo SDK root: {sdk}")

    for relative, recorded in source_commits.items():
        if not isinstance(recorded, str):
            raise RuntimeError(f"invalid recorded source commit: {relative}")
        dirty_record = recorded.endswith("-dirty")
        expected = recorded.removesuffix("-dirty")
        repository = sdk if relative == "bouffalo_sdk" else sdk / relative
        if not repository.is_dir():
            raise RuntimeError(f"recorded source repository is missing: {relative}")
        actual = git_value(repository, "rev-parse", "HEAD")
        if actual != expected:
            raise RuntimeError(
                f"source commit mismatch for {relative}: "
                f"manifest={expected}, actual={actual}"
            )
        if dirty_record and not allow_dirty:
            raise RuntimeError(
                f"manifest records dirty source for {relative}; "
                "use --allow-dirty only for development verification"
            )
        if not dirty_record and repository_is_dirty(repository) and not allow_dirty:
            raise RuntimeError(
                f"source repository is dirty for {relative}; "
                "clean it or use --allow-dirty only for development verification"
            )


def verify_bundle(
    bundle: Path,
    sdk: Path | None,
    allow_dirty: bool,
    source_manifest: Path,
    proprietary_manifest: Path,
) -> None:
    manifest_path = bundle / "manifest.json"
    if not manifest_path.is_file():
        raise RuntimeError(f"manifest is missing: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    if manifest.get("schema") != 4:
        raise RuntimeError(f"unsupported manifest schema: {manifest.get('schema')!r}")
    if manifest.get("scope") != "chip-runtime":
        raise RuntimeError("manifest scope is not chip-runtime")
    if manifest.get("chip") != "bl616cl" or manifest.get("profile") != "bridge":
        raise RuntimeError("bundle is not the BL616CL bridge profile")

    defconfig = bundle / "defconfig"
    if not defconfig.is_file():
        raise RuntimeError("bridge defconfig is missing")
    if manifest.get("defconfig_sha256") != sha256(defconfig):
        raise RuntimeError("bridge defconfig SHA-256 mismatch")

    verify_manifest_files(bundle, manifest)

    source_patches = manifest.get("source_patches")
    if not isinstance(source_patches, dict):
        raise RuntimeError("runtime manifest source_patches field is missing")
    patch_manifest_relative = source_patches.get("manifest")
    patch_manifest_sha256 = source_patches.get("manifest_sha256")
    patch_records = source_patches.get("patches")
    if (
        not isinstance(patch_manifest_relative, str) or
        not isinstance(patch_manifest_sha256, str) or
        not isinstance(patch_records, list) or
        not patch_records
    ):
        raise RuntimeError("runtime manifest source_patches field is invalid")
    platform_root = bundle.parents[3]
    patch_manifest = platform_root / patch_manifest_relative
    if not patch_manifest.is_file():
        raise RuntimeError(f"source patch manifest is missing: {patch_manifest}")
    if sha256(patch_manifest) != patch_manifest_sha256:
        raise RuntimeError("source patch manifest SHA-256 mismatch")
    patch_manifest_data = json.loads(patch_manifest.read_text(encoding="utf-8"))
    if patch_manifest_data.get("schema") != 1:
        raise RuntimeError("source patch manifest schema is invalid")
    manifest_patch_ids = {
        record.get("id")
        for record in patch_records
        if isinstance(record, dict)
    }
    source_patch_ids = {
        record.get("id")
        for record in patch_manifest_data.get("patches", [])
        if isinstance(record, dict)
    }
    if manifest_patch_ids != source_patch_ids:
        raise RuntimeError("runtime/source patch manifest ids differ")
    for record in patch_records:
        if not isinstance(record, dict):
            raise RuntimeError("runtime source patch record is invalid")
        patch_relative = record.get("path")
        patch_hash = record.get("sha256")
        if not isinstance(patch_relative, str) or not isinstance(patch_hash, str):
            raise RuntimeError("runtime source patch metadata is invalid")
        patch_path = platform_root / patch_relative
        if not patch_path.is_file() or sha256(patch_path) != patch_hash:
            raise RuntimeError(f"source patch hash mismatch: {patch_relative}")
        targets = record.get("targets")
        if not isinstance(targets, list) or not targets:
            raise RuntimeError(f"source patch targets are missing: {patch_relative}")
        for target in targets:
            if not isinstance(target, dict):
                raise RuntimeError(f"source patch target is invalid: {patch_relative}")
            if (
                not isinstance(target.get("path"), str) or
                not isinstance(target.get("before_sha256"), str) or
                not isinstance(target.get("after_sha256"), str)
            ):
                raise RuntimeError(f"source patch target metadata is invalid: {patch_relative}")

    for relative in REQUIRED_HEADERS:
        if not (bundle / relative).is_file():
            raise RuntimeError(f"required bridge header is missing: {relative}")
    for library in REQUIRED_LIBRARIES:
        relative = f"lib/{library}"
        if not (bundle / relative).is_file():
            raise RuntimeError(f"required bridge archive is missing: {relative}")
        if relative not in manifest["files"]:
            raise RuntimeError(f"required bridge archive is not in manifest: {relative}")

    source_commits = manifest.get("source_commits")
    if not isinstance(source_commits, dict):
        raise RuntimeError("manifest source_commits field is missing or invalid")
    dirty_records = [
        relative
        for relative, value in source_commits.items()
        if isinstance(value, str) and value.endswith("-dirty")
    ]
    if dirty_records and not allow_dirty:
        raise RuntimeError(
            "bundle is development-only because source_commits contains -dirty: "
            + ", ".join(dirty_records)
        )
    if sdk is not None:
        verify_sdk_commits(sdk, source_commits, allow_dirty)

    verify_dependency_manifests(
        bundle,
        manifest,
        source_manifest,
        proprietary_manifest,
        sdk,
    )

    print(f"BUNDLE={bundle}")
    print(f"PROFILE={manifest['profile']}")
    print(f"SDK_COMMIT={source_commits['bouffalo_sdk']}")
    print(f"MANIFEST_FILE_COUNT={len(manifest['files'])}")
    if dirty_records:
        print("SOURCE_REPRODUCIBILITY=DEVELOPMENT_ONLY")
    else:
        print("SOURCE_REPRODUCIBILITY=REPRODUCIBLE")
    print("BL616CL_RUNTIME_BUNDLE_VERIFY_PASS")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--bundle",
        type=Path,
        default=Path(__file__).resolve().parents[2]
        / "tools"
        / "sdk"
        / "bl616cl"
        / "bridge",
    )
    parser.add_argument(
        "--sdk",
        type=Path,
        default=None,
        help=(
            "SDK checkout to compare with manifest commits; defaults to the "
            "project-controlled third_party/bouffalo_sdk checkout"
        ),
    )
    parser.add_argument(
        "--allow-dirty",
        action="store_true",
        help="allow a development-only bundle or dirty SDK checkout",
    )
    parser.add_argument(
        "--source-manifest",
        type=Path,
        default=Path(__file__).resolve().parent / "source_manifest.json",
        help="source dependency manifest for the bridge runtime",
    )
    parser.add_argument(
        "--proprietary-manifest",
        type=Path,
        default=Path(__file__).resolve().parent / "proprietary_manifest.json",
        help="supplier binary and ROM ABI manifest for the bridge runtime",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    script_dir = Path(__file__).resolve().parent
    sdk = (
        args.sdk.expanduser().resolve()
        if args.sdk is not None
        else controlled_sdk_path(script_dir)
    )
    if not (sdk / "project.build").is_file():
        if args.sdk is None:
            raise RuntimeError(
                f"project-controlled Bouffalo SDK is not initialized: {sdk}\n"
                "Run: python3 tools/prepare_bouffalo_sdk_source.py --init"
            )
        raise RuntimeError(
            f"{sdk} does not look like a BouffaloSDK root "
            f"(missing project.build)"
        )
    verify_bundle(
        args.bundle.resolve(),
        sdk,
        args.allow_dirty,
        args.source_manifest.resolve(),
        args.proprietary_manifest.resolve(),
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
