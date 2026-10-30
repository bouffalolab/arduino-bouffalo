#!/usr/bin/env python3
"""Open the paired-board RA4M1 user-UART runtime route.

The normal route is the verified direct data path:

    Host CDC -> BL616CL GPIO34 -> RA4M1 RX
    RA4M1 TX -> short relay path -> BL616CL GPIO35 -> Host CDC

The FTDI interface only holds the relay in its RA4M1 runtime position. A
separate legacy receiver may be requested for historical split-UART diagnosis.
"""

from __future__ import annotations

from dataclasses import dataclass

import serial


def open_serial(port: str, baudrate: int, timeout: float) -> serial.Serial:
    link = serial.Serial(
        port,
        baudrate=baudrate,
        timeout=timeout,
        write_timeout=timeout,
    )
    link.dtr = False
    link.rts = False
    return link


@dataclass
class RuntimeUartRoute:
    bridge: serial.Serial
    receiver: serial.Serial
    relay: serial.Serial
    direct: bool

    def close(self) -> None:
        for link in {id(self.bridge): self.bridge, id(self.receiver): self.receiver, id(self.relay): self.relay}.values():
            if link.is_open:
                if link is self.relay:
                    link.rts = False
                link.close()


def open_runtime_route(
    bridge_port: str,
    relay_port: str,
    legacy_reference_port: str | None = None,
    *,
    bridge_baudrate: int = 115200,
    bridge_timeout: float = 1.0,
    receiver_timeout: float = 0.05,
) -> RuntimeUartRoute:
    """Select the runtime route and return logical TX and RX serial endpoints.

    When ``legacy_reference_port`` is absent, ``bridge`` and ``receiver`` are
    the same serial object. Callers must use ``RuntimeUartRoute.close()`` so
    that the shared direct endpoint is closed only once.
    """
    relay = open_serial(relay_port, 115200, 0.2)
    relay.rts = False
    bridge = None
    receiver = None
    try:
        bridge = open_serial(bridge_port, bridge_baudrate, bridge_timeout)
        if legacy_reference_port is None:
            receiver = bridge
        elif legacy_reference_port == relay_port:
            receiver = relay
            receiver.baudrate = bridge_baudrate
            receiver.timeout = receiver_timeout
            receiver.write_timeout = receiver_timeout
        else:
            receiver = open_serial(
                legacy_reference_port, bridge_baudrate, receiver_timeout
            )
        return RuntimeUartRoute(
            bridge=bridge,
            receiver=receiver,
            relay=relay,
            direct=legacy_reference_port is None,
        )
    except BaseException:
        for link in (receiver, bridge, relay):
            if link is not None and link.is_open:
                if link is relay:
                    link.rts = False
                link.close()
        raise
