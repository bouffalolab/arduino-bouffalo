"""Reject incomplete/duplicated accounting and retain failed receive boundaries."""
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
from summarize_ra4_tls_receive_budget import decode

boundary = 'RA4_TLS_RESOURCE:RX_BURST_BOUNDARY=1,10012,7\n'
rows = ''.join(f'RA4_TLS_RESOURCE:RX_BURST_SLOT_BUDGET=1,{s},20,5,0,250,250,20,5,900,900,4096\n'
               for s in range(2000, 2008))
report = decode(boundary+rows)[0]
assert report['completed'] == 7  # Accounting must never upgrade a failed deadline.
assert report['outside_calls_ms'] == 812
assert report['plaintext_wire_lower_bound_ms'] == 2844.44
assert report['totals']['refill_calls'] == 40
for invalid in ('', boundary, rows, boundary+rows+rows,
                boundary+boundary+rows, (boundary+rows).replace(',20,5,0,', ',20,21,0,'),
                (boundary+rows).replace(',10012,7', ',100,7')):
    try:
        decode(invalid)
    except ValueError:
        pass
    else:
        raise AssertionError('invalid evidence accepted')
print('RA4_TLS_RECEIVE_BUDGET_PASS')
