#!/usr/bin/env python3
"""Verify the BL616CL bridge source and supplier-binary dependency manifests."""

from __future__ import annotations

import argparse
import hashlib
import json
import sys
from pathlib import Path


EXPECTED_LINK_ARCHIVES = {
    "lib/libbtblecontroller_bl616cl_m2s1.a",
    "lib/libbl616cl_phyrf.a",
    "lib/libblestack.a",
    "lib/libcherryusb.a",
    "lib/libdebug.a",
    "lib/libfhost_bl616cl_default.a",
    "lib/libfhost_config.a",
    "lib/libfreertos.a",
    "lib/liblhal.a",
    "lib/liblibc.a",
    "lib/liblittlefs.a",
    "lib/liblwip.a",
    "lib/libmacsw_bl616cl.a",
    "lib/libmacsw_config_bl616cl_default.a",
    "lib/libmacsw_os_adapter.a",
    "lib/libmacsw_plat.a",
    "lib/libmbedtls.a",
    "lib/libmm.a",
    "lib/libping.a",
    "lib/libpka_bl616cl.a",
    "lib/librfparam.a",
    "lib/libstd.a",
    "lib/libsys.a",
    "lib/libsysinit.a",
    "lib/libutils.a",
    "lib/libwifi6_lwip_adapter.a",
    "lib/libwpa_supplicant.a",
    "lib_board/libapp.a",
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


def load_json(path: Path, description: str) -> dict[str, object]:
    if not path.is_file():
        raise RuntimeError(f"{description} is missing: {path}")
    data = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise RuntimeError(f"{description} must be a JSON object")
    return data


def require_text(value: object, field: str) -> str:
    if not isinstance(value, str) or not value:
        raise RuntimeError(f"{field} is missing or invalid")
    return value


def require_string_list(value: object, field: str) -> list[str]:
    if not isinstance(value, list) or not value:
        raise RuntimeError(f"{field} is missing or invalid")
    if not all(isinstance(item, str) and item for item in value):
        raise RuntimeError(f"{field} contains an invalid entry")
    return value


def verify_manifest_header(
    manifest: dict[str, object],
    expected_scope: str,
    description: str,
) -> None:
    if manifest.get("schema") != 1:
        raise RuntimeError(
            f"{description} has unsupported schema: {manifest.get('schema')!r}"
        )
    if manifest.get("scope") != expected_scope:
        raise RuntimeError(f"{description} scope is invalid")
    if manifest.get("chip") != "bl616cl" or manifest.get("profile") != "bridge":
        raise RuntimeError(f"{description} does not describe the BL616CL bridge")
    require_text(manifest.get("sdk_commit"), f"{description}.sdk_commit")


def verify_source_record(
    record: object,
    bundle: Path,
    bundle_manifest: dict[str, object],
    sdk: Path | None,
) -> str:
    if not isinstance(record, dict):
        raise RuntimeError("source manifest archive record is invalid")
    archive = require_text(record.get("archive"), "source archive")
    if archive not in EXPECTED_LINK_ARCHIVES:
        raise RuntimeError(f"source manifest lists a non-link archive: {archive}")
    source_paths = require_string_list(record.get("source_paths"), f"{archive}.source_paths")
    require_string_list(record.get("public_apis"), f"{archive}.public_apis")
    require_string_list(record.get("cmake_files"), f"{archive}.cmake_files")
    require_text(record.get("license"), f"{archive}.license")
    if record.get("classification") != "source":
        raise RuntimeError(f"{archive} must have source classification")

    bundle_file = bundle / archive
    if not bundle_file.is_file():
        raise RuntimeError(f"source archive is absent from bundle: {archive}")
    files = bundle_manifest.get("files")
    if not isinstance(files, dict) or archive not in files:
        raise RuntimeError(f"source archive is absent from runtime manifest: {archive}")
    metadata = files[archive]
    if not isinstance(metadata, dict) or metadata.get("sha256") != sha256(bundle_file):
        raise RuntimeError(f"source archive runtime-manifest hash mismatch: {archive}")

    if sdk is not None:
        for relative in [*source_paths, *require_string_list(record.get("cmake_files"), f"{archive}.cmake_files")]:
            if not (sdk / relative).exists():
                raise RuntimeError(f"SDK source input is missing for {archive}: {relative}")
    return archive


def verify_prebuilt_record(
    record: object,
    bundle: Path,
    bundle_manifest: dict[str, object],
    sdk: Path | None,
) -> str:
    if not isinstance(record, dict):
        raise RuntimeError("proprietary manifest artifact record is invalid")
    archive = require_text(record.get("archive"), "prebuilt archive")
    if archive not in EXPECTED_LINK_ARCHIVES:
        raise RuntimeError(f"proprietary manifest lists a non-link archive: {archive}")
    if record.get("classification") != "supplier-prebuilt":
        raise RuntimeError(f"{archive} must have supplier-prebuilt classification")
    require_text(record.get("reason"), f"{archive}.reason")
    require_text(record.get("license_status"), f"{archive}.license_status")
    source_path = require_text(record.get("sdk_source_path"), f"{archive}.sdk_source_path")
    expected_hash = require_text(record.get("sha256"), f"{archive}.sha256")

    bundle_file = bundle / archive
    if not bundle_file.is_file():
        raise RuntimeError(f"prebuilt archive is absent from bundle: {archive}")
    actual_hash = sha256(bundle_file)
    if actual_hash != expected_hash:
        raise RuntimeError(f"prebuilt archive hash mismatch: {archive}")
    files = bundle_manifest.get("files")
    if not isinstance(files, dict) or archive not in files:
        raise RuntimeError(f"prebuilt archive is absent from runtime manifest: {archive}")
    metadata = files[archive]
    if not isinstance(metadata, dict) or metadata.get("sha256") != actual_hash:
        raise RuntimeError(f"prebuilt archive runtime-manifest hash mismatch: {archive}")
    if sdk is not None and not (sdk / source_path).is_file():
        raise RuntimeError(f"SDK prebuilt artifact is missing for {archive}: {source_path}")
    return archive


def verify_rom_boundary(record: object, sdk: Path | None) -> None:
    if not isinstance(record, dict):
        raise RuntimeError("ROM boundary record is invalid")
    if record.get("classification") != "chip-rom-abi":
        raise RuntimeError("ROM boundary classification is invalid")
    require_text(record.get("name"), "ROM boundary name")
    require_text(record.get("reason"), "ROM boundary reason")
    source_paths = require_string_list(record.get("source_paths"), "ROM boundary source_paths")
    if sdk is not None:
        for relative in source_paths:
            if not (sdk / relative).is_file():
                raise RuntimeError(f"SDK ROM ABI source input is missing: {relative}")


def verify_dependency_manifests(
    bundle: Path,
    bundle_manifest: dict[str, object],
    source_manifest_path: Path,
    proprietary_manifest_path: Path,
    sdk: Path | None,
) -> None:
    source_manifest = load_json(source_manifest_path, "source manifest")
    proprietary_manifest = load_json(proprietary_manifest_path, "proprietary manifest")
    verify_manifest_header(source_manifest, "bl616cl-bridge-source-inputs", "source manifest")
    verify_manifest_header(
        proprietary_manifest,
        "bl616cl-bridge-proprietary-boundaries",
        "proprietary manifest",
    )

    bundle_commits = bundle_manifest.get("source_commits")
    if not isinstance(bundle_commits, dict):
        raise RuntimeError("runtime manifest source_commits is missing or invalid")
    bundle_sdk_commit = require_text(bundle_commits.get("bouffalo_sdk"), "runtime SDK commit")
    for manifest, description in (
        (source_manifest, "source manifest"),
        (proprietary_manifest, "proprietary manifest"),
    ):
        if manifest["sdk_commit"] != bundle_sdk_commit.removesuffix("-dirty"):
            raise RuntimeError(f"{description} SDK commit does not match runtime bundle")

    source_records = source_manifest.get("archives")
    if not isinstance(source_records, list) or not source_records:
        raise RuntimeError("source manifest archives is missing or invalid")
    prebuilt_records = proprietary_manifest.get("artifacts")
    if not isinstance(prebuilt_records, list) or not prebuilt_records:
        raise RuntimeError("proprietary manifest artifacts is missing or invalid")

    classified = set()
    for record in source_records:
        archive = verify_source_record(record, bundle, bundle_manifest, sdk)
        if archive in classified:
            raise RuntimeError(f"duplicate dependency classification: {archive}")
        classified.add(archive)
    for record in prebuilt_records:
        archive = verify_prebuilt_record(record, bundle, bundle_manifest, sdk)
        if archive in classified:
            raise RuntimeError(f"duplicate dependency classification: {archive}")
        classified.add(archive)

    missing = sorted(EXPECTED_LINK_ARCHIVES - classified)
    extra = sorted(classified - EXPECTED_LINK_ARCHIVES)
    if missing or extra:
        details = []
        if missing:
            details.append("unclassified=" + ", ".join(missing))
        if extra:
            details.append("unexpected=" + ", ".join(extra))
        raise RuntimeError("link dependency classification is incomplete: " + "; ".join(details))

    rom_boundaries = proprietary_manifest.get("rom_boundaries")
    if not isinstance(rom_boundaries, list) or not rom_boundaries:
        raise RuntimeError("proprietary manifest ROM boundaries is missing or invalid")
    for record in rom_boundaries:
        verify_rom_boundary(record, sdk)

    print(f"SOURCE_MANIFEST={source_manifest_path}")
    print(f"PROPRIETARY_MANIFEST={proprietary_manifest_path}")
    print(f"CLASSIFIED_LINK_ARCHIVES={len(classified)}")
    print(f"SUPPLIER_PREBUILT_ARCHIVES={len(prebuilt_records)}")
    print(f"ROM_ABI_BOUNDARIES={len(rom_boundaries)}")
    print("BL616CL_SOURCE_BOUNDARY_VERIFY_PASS")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    script_dir = Path(__file__).resolve().parent
    parser.add_argument(
        "--bundle",
        type=Path,
        default=script_dir.parent / "sdk" / "bl616cl" / "bridge",
    )
    parser.add_argument(
        "--source-manifest",
        type=Path,
        default=script_dir / "source_manifest.json",
    )
    parser.add_argument(
        "--proprietary-manifest",
        type=Path,
        default=script_dir / "proprietary_manifest.json",
    )
    parser.add_argument(
        "--sdk",
        type=Path,
        default=None,
        help=(
            "SDK checkout used to validate recorded source paths; defaults to "
            "the project-controlled third_party/bouffalo_sdk checkout"
        ),
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
    bundle = args.bundle.resolve()
    bundle_manifest = load_json(bundle / "manifest.json", "runtime manifest")
    verify_dependency_manifests(
        bundle,
        bundle_manifest,
        args.source_manifest.resolve(),
        args.proprietary_manifest.resolve(),
        sdk,
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
