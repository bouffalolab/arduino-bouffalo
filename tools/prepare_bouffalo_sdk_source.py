#!/usr/bin/env python3
"""Initialize and verify the project-controlled Bouffalo SDK source input."""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
from pathlib import Path


ROOT_DIR = Path(__file__).resolve().parents[1]
LOCK_PATH = ROOT_DIR / "third_party" / "bouffalo_sdk.lock.json"


def run(command: list[str], *, cwd: Path | None = None) -> None:
    print("+ " + " ".join(command))
    subprocess.run(command, cwd=cwd, check=True)


def capture(command: list[str], *, cwd: Path | None = None) -> str:
    result = subprocess.run(
        command,
        cwd=cwd,
        check=True,
        text=True,
        stdout=subprocess.PIPE,
    )
    return result.stdout.strip()


def load_lock() -> dict[str, object]:
    data = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
    if not isinstance(data, dict) or data.get("schema") != 1:
        raise RuntimeError("unsupported Bouffalo SDK source lock")
    if not isinstance(data.get("path"), str) or not isinstance(data.get("commit"), str):
        raise RuntimeError("Bouffalo SDK source lock is missing path or commit")
    if not isinstance(data.get("source_url"), str):
        raise RuntimeError("Bouffalo SDK source lock is missing source_url")
    bootstrap = data.get("bootstrap")
    if not isinstance(bootstrap, dict):
        raise RuntimeError("Bouffalo SDK source lock is missing bootstrap")
    sparse_paths = bootstrap.get("sparse_paths")
    if not isinstance(sparse_paths, list) or not all(
        isinstance(path, str) and path for path in sparse_paths
    ):
        raise RuntimeError("Bouffalo SDK source lock has invalid sparse_paths")
    submodules = data.get("required_submodules")
    if not isinstance(submodules, dict) or not submodules:
        raise RuntimeError("Bouffalo SDK source lock has no required_submodules")
    return data


def source_path(lock: dict[str, object]) -> Path:
    return ROOT_DIR / str(lock["path"])


def ensure_root_submodule(lock: dict[str, object], destination: Path) -> None:
    if destination.exists():
        if not (destination / ".git").exists():
            raise RuntimeError(
                f"{destination} exists but is not a Git checkout; "
                "move it aside before initializing the pinned source input"
            )
        return

    path = str(lock["path"])
    expected = str(lock["commit"])
    entry = capture(["git", "ls-files", "--stage", "--", path], cwd=ROOT_DIR)
    expected_entry = f"160000 {expected} 0\t{path}"
    if entry != expected_entry:
        raise RuntimeError(
            "project Git index is missing the locked Bouffalo SDK gitlink: "
            f"expected={expected_entry!r}, actual={entry!r}"
        )
    run(
        [
            "git",
            "clone",
            "--depth",
            "1",
            "--filter=blob:none",
            "--no-checkout",
            str(lock["source_url"]),
            str(destination),
        ]
    )
    run(["git", "checkout", "--detach", str(lock["commit"])], cwd=destination)


def configure_sparse_checkout(lock: dict[str, object], destination: Path) -> None:
    sparse_paths = list(lock["bootstrap"]["sparse_paths"])  # type: ignore[index]
    run(["git", "sparse-checkout", "init", "--cone"], cwd=destination)
    run(["git", "sparse-checkout", "set", "--cone", *sparse_paths], cwd=destination)


def initialize_required_submodules(lock: dict[str, object], destination: Path) -> None:
    submodules = lock["required_submodules"]
    assert isinstance(submodules, dict)
    paths = sorted(
        (str(path) for path in submodules),
        key=lambda path: (path.count("/"), path),
    )
    for path in paths:
        parents = [
            parent
            for parent in paths
            if parent != path and path.startswith(parent + "/")
        ]
        parent = max(parents, key=len) if parents else None
        repository = destination if parent is None else destination / parent
        relative_path = path if parent is None else path.removeprefix(parent + "/")
        run(
            [
                "git",
                "submodule",
                "update",
                "--init",
                "--depth",
                "1",
                "--",
                relative_path,
            ],
            cwd=repository,
        )


def require_commit(repository: Path, expected: str, description: str) -> None:
    if not repository.is_dir():
        raise RuntimeError(f"{description} is missing: {repository}")
    actual = capture(["git", "rev-parse", "HEAD"], cwd=repository)
    if actual != expected:
        raise RuntimeError(f"{description} commit mismatch: expected={expected}, actual={actual}")


def require_origin(repository: Path, expected: str, description: str) -> None:
    actual = capture(["git", "remote", "get-url", "origin"], cwd=repository)
    if normalize_origin(actual) != normalize_origin(expected):
        raise RuntimeError(
            f"{description} origin mismatch: expected={expected}, actual={actual}"
        )


def normalize_origin(url: str) -> str:
    return url.rstrip("/").removesuffix(".git")


def require_clean(repository: Path, description: str) -> None:
    status = capture(
        ["git", "status", "--porcelain=v1", "--untracked-files=no"],
        cwd=repository,
    )
    if status:
        raise RuntimeError(f"{description} has tracked local changes: {status}")


def verify(lock: dict[str, object]) -> None:
    destination = source_path(lock)
    require_commit(destination, str(lock["commit"]), "Bouffalo SDK")
    require_origin(destination, str(lock["source_url"]), "Bouffalo SDK")
    require_clean(destination, "Bouffalo SDK")

    submodules = lock["required_submodules"]
    assert isinstance(submodules, dict)
    for relative, metadata in sorted(submodules.items()):
        if (
            not isinstance(metadata, dict)
            or not isinstance(metadata.get("commit"), str)
            or not isinstance(metadata.get("url"), str)
        ):
            raise RuntimeError(f"invalid required submodule record: {relative}")
        repository = destination / str(relative)
        require_commit(
            repository,
            str(metadata["commit"]),
            f"Bouffalo SDK submodule {relative}",
        )
        require_origin(
            repository,
            str(metadata["url"]),
            f"Bouffalo SDK submodule {relative}",
        )
        require_clean(repository, f"Bouffalo SDK submodule {relative}")

    print(f"BOUFFALO_SDK_SOURCE={destination}")
    print(f"BOUFFALO_SDK_COMMIT={lock['commit']}")
    print(f"BOUFFALO_SDK_REQUIRED_SUBMODULES={len(submodules)}")
    print("BOUFFALO_SDK_SOURCE_LOCK_VERIFY_PASS")


def parse_arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    action = parser.add_mutually_exclusive_group(required=True)
    action.add_argument(
        "--init",
        action="store_true",
        help="clone the pinned source, apply sparse checkout, and initialize the required submodules",
    )
    action.add_argument(
        "--check",
        action="store_true",
        help="verify the existing project-controlled source against the lock",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_arguments()
    lock = load_lock()
    destination = source_path(lock)
    if args.init:
        ensure_root_submodule(lock, destination)
        configure_sparse_checkout(lock, destination)
        initialize_required_submodules(lock, destination)
    verify(lock)
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, RuntimeError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        raise SystemExit(1)
