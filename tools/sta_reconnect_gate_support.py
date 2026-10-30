"""Shared non-secret helpers for controlled STA reconnect gates."""

from __future__ import annotations

import os
import subprocess
from pathlib import Path
from typing import Callable


def append_status_if_changed(statuses: list[int], status: int) -> None:
    if not statuses or statuses[-1] != status:
        statuses.append(status)


def status_sequence_text(statuses: list[int]) -> str:
    return ",".join(map(str, statuses))


def require_hook(path_text: str | None, label: str) -> Path:
    if not path_text:
        raise RuntimeError(f"{label} hook must be provided")
    path = Path(path_text)
    if not path.is_file():
        raise RuntimeError(f"{label} hook is not a regular file")
    if not os.access(path, os.X_OK):
        raise RuntimeError(f"{label} hook is not executable")
    return path


def run_hook(
    path: Path,
    phase: str,
    timeout_seconds: float,
    control_socket: str | None = None,
) -> None:
    if phase not in ("outage", "recovery"):
        raise RuntimeError("unsupported AP hook phase")
    if timeout_seconds <= 0:
        raise RuntimeError("AP hook timeout must be positive")

    environment = os.environ.copy()
    environment.pop("BL616CL_STA_SSID", None)
    environment.pop("BL616CL_STA_PASSWORD", None)
    environment["UNO_R4_STA_RECONNECT_PHASE"] = phase
    if control_socket is not None:
        environment["UNO_R4_SOFTAP_CONTROL_SOCKET"] = control_socket
    try:
        result = subprocess.run(
            [str(path)],
            check=False,
            stdin=subprocess.DEVNULL,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=environment,
            timeout=timeout_seconds,
        )
    except subprocess.TimeoutExpired as error:
        raise RuntimeError(f"AP {phase} hook timed out") from error
    if result.returncode != 0:
        raise RuntimeError(f"AP {phase} hook failed with exit status {result.returncode}")


def observe_statuses(
    read_status: Callable[[], int],
    duration_seconds: float,
    poll_interval_seconds: float,
    monotonic: Callable[[], float],
    sleep: Callable[[float], None],
) -> list[int]:
    if duration_seconds <= 0 or poll_interval_seconds <= 0:
        raise RuntimeError("status observation timing must be positive")

    deadline = monotonic() + duration_seconds
    observed: list[int] = []
    while monotonic() < deadline:
        append_status_if_changed(observed, read_status())
        sleep(poll_interval_seconds)
    return observed


def require_outage_statuses(statuses: list[int]) -> None:
    if 0 not in statuses:
        raise RuntimeError("AP outage did not produce GETSTATUS=0")
    disconnected_index = statuses.index(0)
    if 3 in statuses[disconnected_index + 1 :]:
        raise RuntimeError("GETSTATUS returned to 3 before AP recovery hook ran")
