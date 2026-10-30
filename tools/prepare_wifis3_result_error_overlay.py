#!/usr/bin/env python3
"""Create a temporary, checked WiFiS3 core-fix overlay without changing Arduino data."""

from __future__ import annotations

import argparse
import hashlib
import shutil
from pathlib import Path


ARDUINO_CORE_VERSION = "1.6.0"
MODEM_SOURCE_SHA256 = (
    "2c2aa923ea176cf723932e55f5afedf9af25de4b7ba738595f068bc0b2caad26"
)
BROKEN_DEFINE = "#define RESULT_ERROR    OK_TOKEN TERM_TOKEN"
FIXED_DEFINE = "#define RESULT_ERROR    ERROR_TOKEN TERM_TOKEN"
BROKEN_BARE_ERROR_RESTART = (
    "if(restart && state == at_parse_state_t::Completed) {"
)
FIXED_BARE_ERROR_RESTART = (
    "if(restart && state == at_parse_state_t::Completed && res != Error) {"
)


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(65536), b""):
            digest.update(chunk)
    return digest.hexdigest()


def patch_modem_source(source: str) -> str:
    if FIXED_DEFINE in source or FIXED_BARE_ERROR_RESTART in source:
        raise RuntimeError("WiFiS3 Modem.cpp already contains the fixed define")
    if (
        source.count(BROKEN_DEFINE) != 1
        or source.count(BROKEN_BARE_ERROR_RESTART) != 1
    ):
        raise RuntimeError(
            "WiFiS3 Modem.cpp does not contain the expected broken parser"
        )
    return source.replace(BROKEN_DEFINE, FIXED_DEFINE).replace(
        BROKEN_BARE_ERROR_RESTART,
        FIXED_BARE_ERROR_RESTART,
    )



def patch_sized_response_reserve(source: str) -> str:
    """Optional RA4 diagnostic: reserve a complete default-client response once.

    Only known lengths <= 1024 are affected. Preserve byte collection, framing,
    response length, FIFO sizes and timeout; larger responses keep core behavior.
    """
    before = """            data_res.clear();
            if (sized_read_size != 0) {"""
    after = """            data_res.clear();
            // Diagnostic overlay: avoid geometric growth for a known FIFO response.
            if (sized_read_size > 0U && sized_read_size <= 1024U) {
               data_res.reserve(sized_read_size);
            }
            if (sized_read_size != 0) {"""
    if source.count(before) != 1:
        raise RuntimeError("WiFiS3 sized-response reserve anchor mismatch")
    return source.replace(before, after, 1)


def default_source_library() -> Path:
    return (
        Path.home()
        / ".arduino15"
        / "packages"
        / "arduino"
        / "hardware"
        / "renesas_uno"
        / ARDUINO_CORE_VERSION
        / "libraries"
        / "WiFiS3"
    )


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--source-library",
        type=Path,
        default=default_source_library(),
        help="installed, unmodified WiFiS3 library to verify and copy",
    )
    parser.add_argument(
        "--output-library",
        type=Path,
        required=True,
        help="new temporary WiFiS3 library directory",
    )
    parser.add_argument("--sized-response-reserve", action="store_true",
                        help="optional RA4 diagnostic: reserve known binary responses up to 1024 bytes")
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    source_library = args.source_library.resolve()
    output_library = args.output_library.resolve()
    source_modem = source_library / "src" / "Modem.cpp"
    source_properties = source_library / "library.properties"

    if not source_modem.is_file() or not source_properties.is_file():
        raise RuntimeError("expected Arduino renesas_uno 1.6.0 WiFiS3 library is unavailable")
    properties = source_properties.read_text(encoding="ascii")
    if "name=WiFiS3\n" not in properties or "architectures=renesas,renesas_uno" not in properties:
        raise RuntimeError("source library is not the expected WiFiS3 Arduino library")
    observed_sha256 = sha256_file(source_modem)
    if observed_sha256 != MODEM_SOURCE_SHA256:
        raise RuntimeError(
            "refusing to patch an unrecognized WiFiS3 Modem.cpp "
            f"(expected {MODEM_SOURCE_SHA256}, got {observed_sha256})"
        )
    if output_library.exists():
        raise RuntimeError(f"output library already exists: {output_library}")

    shutil.copytree(source_library, output_library)
    output_modem = output_library / "src" / "Modem.cpp"
    patched = patch_modem_source(output_modem.read_text(encoding="ascii"))
    if args.sized_response_reserve:
        patched = patch_sized_response_reserve(patched)
    output_modem.write_text(patched, encoding="ascii")
    patched_source = output_modem.read_text(encoding="ascii")
    if (
        FIXED_DEFINE not in patched_source
        or FIXED_BARE_ERROR_RESTART not in patched_source
    ):
        raise RuntimeError("WiFiS3 overlay patch verification failed")

    print(f"WIFIS3_RESULT_ERROR_OVERLAY={output_library}")
    print(f"WIFIS3_RESULT_ERROR_SOURCE_SHA256={observed_sha256}")
    if args.sized_response_reserve:
        print("WIFIS3_SIZED_RESPONSE_RESERVE_OVERLAY_PASS=1024")
    print("WIFIS3_RESULT_ERROR_OVERLAY_PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
