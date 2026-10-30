#!/usr/bin/env python3
"""A payload containing K# must never count as an application handoff."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from run_ra4_bossac_direct import DirectSerialProxy
class Link:
    def __init__(self): self.writes = []
    def write(self, data):
        self.writes.append(bytes(data))
        return len(data)
    def flush(self): pass
link = Link()
commands = []
proxy = DirectSerialProxy(-1, link, 64, 0, commands.append)
proxy.tx_buffer.extend(b'N#S20000000,00000002#K#')
proxy.flush_tx()
assert commands == [b'N#', b'S20000000,00000002#']
assert proxy.pending_bytes == 0
proxy.tx_buffer.extend(b'K')
proxy.flush_tx()
assert commands[-1] != b'K#' and proxy.pending_bytes == 1
proxy.tx_buffer.extend(b'#')
proxy.flush_tx()
assert commands[-1] == b'K#' and commands.count(b'K#') == 1
assert proxy.pending_bytes == 0
assert b''.join(link.writes) == b'N#S20000000,00000002#K#K#'
print('RA4_BOSSAC_HANDOFF_PROXY_PASS')
