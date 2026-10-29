from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parents[1]/'tools'))
from summarize_bl616cl_tls_flow import decode,FLOW_FIELDS
prefix='RA4_TLS_RESOURCE:RX_BURST_INCOMPLETE_DIAG='
base=[43,0]+[0]*42
flow=[1,1,50004,4,123,2048,2048,2171,100,101,0,0,0,0,0xffffffff,1023,0,1]
assert len(flow)==len(FLOW_FIELDS)
assert decode(prefix+','.join(map(str,base)))==[]
record=decode('RA4_TLS_RESOURCE:RX_BURST_INCOMPLETE_SLOT=2004\r\n'+prefix+','.join(map(str,base+flow))+'\r\n')[0]
assert record['tls_slot']==2004 and record['phase']==43 and record['flow']['rtime']==-1
assert record['flow']['tls_readable']==1023 and record['flow']['rcv_nxt']==123
for invalid in (base+flow[:-1],base+[2]+flow[1:],base+flow+[0]):
 try:decode(prefix+','.join(map(str,invalid)))
 except ValueError:pass
 else:raise AssertionError('unsupported diagnostic accepted')
print('BL616CL_TLS_FLOW_DECODER_PASS')
