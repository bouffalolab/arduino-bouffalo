"""Deterministic relay selection for the paired-board RA4M1 UART route."""

from __future__ import annotations

import time

import serial


def select_ra4_runtime_route(
    relay: serial.Serial,
    settle_ms: int,
    *,
    transition_ms: int = 50,
) -> None:
    """Select the BL616CL <-> RA4M1 UART path from a known relay state.

    The relay's active state connects the CKLink download path. Force that
    state briefly, then release it before opening the bridge CDC endpoint.
    This avoids treating a fresh FTDI open's RTS default as a deterministic
    physical route.
    """
    if settle_ms < 0 or transition_ms < 0:
        raise ValueError("relay settle times must not be negative")

    relay.rts = True
    time.sleep(transition_ms / 1000)
    relay.rts = False
    time.sleep(settle_ms / 1000)
