"""Read only newlib-nano free chunk headers from an already halted RA4 target.

The renesas_uno 1.6.0 allocator uses size/next uint32 words, as audited in
_malloc_r. Never traverse allocated contents or print former user payload.
"""
import re
import socket


def summarize_free_list(read_word, head, base, end, limit, max_nodes=256):
    if not 0x20000000 <= base <= end <= limit <= 0x20008000:
        raise ValueError("invalid RA4 heap boundaries")
    visited = set()
    total = largest = 0
    while head:
        if len(visited) >= max_nodes:
            raise ValueError("free list exceeds diagnostic bound")
        if head in visited or head & 3 or not base <= head <= end - 8:
            raise ValueError("invalid free chunk pointer or cycle")
        visited.add(head)
        size = read_word(head)
        if size < 12 or size & 3 or head + size > end:
            raise ValueError("invalid free chunk size")
        next_head = read_word(head + 4)
        total += size
        largest = max(largest, size)
        head = next_head
    return {"complete": True, "free_chunks": len(visited),
            "total_free_chunk_bytes": total, "largest_free_chunk_bytes": largest,
            "unextended_bytes": limit - end, "heap_capacity_bytes": limit - base}


class TclWordReader:
    def __init__(self, port=6666):
        self.link = socket.create_connection(("127.0.0.1", port), timeout=2)
        self.link.settimeout(2)

    def close(self):
        self.link.close()

    def __call__(self, address):
        self.link.sendall(("mdw 0x%x 1\x1a" % address).encode("ascii"))
        data = bytearray()
        while b"\x1a" not in data:
            chunk = self.link.recv(512)
            if not chunk:
                raise ValueError("OpenOCD connection closed")
            data.extend(chunk)
            if len(data) > 4096:
                raise ValueError("unexpected OpenOCD read response")
        text = bytes(data).decode("ascii", errors="replace")
        match = re.fullmatch(r"\s*0x([0-9a-fA-F]+):\s+([0-9a-fA-F]{8})\s*\x1a", text)
        if not match or int(match[1], 16) != address:
            raise ValueError("OpenOCD did not return the requested metadata word")
        return int(match[2], 16)
