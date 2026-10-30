#!/usr/bin/env python3
"""Guard TCP peer reset/FIN visibility in the BL616CL socket backend."""

from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = (
    ROOT / "UNOR4BL616CLBridge" / "bouffalo_tcp_client_backend.cpp"
).read_text(encoding="ascii")


def require(text: str, message: str) -> None:
    if text not in SOURCE:
        raise RuntimeError(message)


require(
    "SocketPeerState probe_socket_peer(int handle)",
    "TCP backend must provide a shared non-consuming peer-state probe",
)
require(
    "remembered_closed_handles",
    "TCP backend must retain a peer failure after SO_ERROR is queried",
)
require(
    "forget_closed_handle(handle);",
    "TCP backend must clear remembered peer failure when a socket closes",
)
require(
    "getsockopt(\n            handle,\n            SOL_SOCKET,\n            SO_ERROR,",
    "TCP peer probe must expose pending lwIP socket errors",
)
require(
    "const int result = recv(handle, &byte, 1U, MSG_PEEK);",
    "TCP peer probe must detect RST/FIN without consuming application data",
)
require(
    "if (errno == EAGAIN || errno == EWOULDBLOCK)",
    "TCP peer probe must keep an idle connection alive",
)
require(
    "if (probe_socket_peer(handle) != SocketPeerState::Alive)",
    "TCP send must reject a peer reset or orderly close before queueing data",
)
if SOURCE.count(
    "const SocketPeerState peer_state = probe_socket_peer(handle);"
) < 2:
    raise RuntimeError(
        "AVAILABLE and CLIENTCONNECTED must use the same peer-state semantics"
    )
require(
    "if (!has_peer) {\n        *connected = false;\n        return true;",
    "CLIENTCONNECTED must leave a never-connected socket reusable",
)
connected = SOURCE.split("bool connected_socket(", 1)[1].split("bool get_state_socket(", 1)[0]
if connected.index("const bool has_peer = getpeername(") > connected.index(
    "const SocketPeerState peer_state = probe_socket_peer(handle);"
):
    raise RuntimeError("CLIENTCONNECTED must identify a peer before probing it")
print("BL616CL_TCP_PEER_FAILURE_SOURCE_PASS")
