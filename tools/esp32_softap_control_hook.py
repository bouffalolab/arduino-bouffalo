#!/usr/bin/env python3
"""Request a state change from a private ESP32-S3 SoftAP controller."""

from __future__ import annotations

import os
import socket
import sys
from pathlib import Path


def main() -> int:
    phase = os.environ.get("UNO_R4_STA_RECONNECT_PHASE")
    socket_text = os.environ.get("UNO_R4_SOFTAP_CONTROL_SOCKET")
    if phase not in ("outage", "recovery") or not socket_text:
        return 1

    path = Path(socket_text)
    if path.parent != Path("/tmp"):
        return 1
    with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
        connection.settimeout(15.0)
        connection.connect(str(path))
        connection.sendall(phase.encode("ascii") + b"\n")
        if connection.recv(16) != b"OK\n":
            return 1
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except OSError:
        raise SystemExit(1)
