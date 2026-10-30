"""An IPv6 address still in DAD may return ERROR before becoming valid."""
import sys
from pathlib import Path
from unittest.mock import patch
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import run_bl616cl_hostname_ipv6_final_carrier as gate
class Link:
    def __init__(self, chunks): self.chunks=iter(chunks)
    def read(self, count): return next(self.chunks)
for chunks, expected in (([b'ER', b'ROR\r\n'], b'ERROR\r\n'), ([b'OK\r\n'], b'OK\r\n')):
    assert gate.read_until_terminal(Link(chunks), 1) == expected
with patch.object(gate, 'transact', side_effect=[b'ERROR\r\n', b'+IPV6: fe80::1\r\nOK\r\n']) as query, patch.object(gate.time, 'sleep'):
    gate.wait_for_link_local_ipv6(None, 1, 1)
    assert query.call_count == 2
try:
    gate.require_exact(b'ERROR\r\n', b'OK\r\n', 'enable')
except RuntimeError:
    pass
else:
    raise AssertionError('ERROR passed an enable assertion')
print('BL616CL_IPV6_POLL_RESPONSE_PASS')
