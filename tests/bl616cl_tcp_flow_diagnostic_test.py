"""Exercise the production flow snapshot with colliding ports and held queues."""
from pathlib import Path
import subprocess,tempfile
ROOT=Path(__file__).resolve().parents[1]
SOURCE=r'''
#include "tcp_flow_diagnostic.h"
#include <cassert>
#include <cstring>
struct Buf { unsigned tot_len; };
struct Seg { Seg *next; Buf *p; };
struct Pcb {
 Pcb *next; unsigned local_port, remote_port, local_ip, remote_ip;
 unsigned state,rcv_nxt,rcv_wnd,rcv_ann_wnd,rcv_ann_right_edge,lastack,snd_nxt;
 Seg *ooseq; Buf *refused_data;unsigned flags,nrtx;int rtime;
};
int main() {
 Buf a{500},b{700},refused{333};Seg two{nullptr,&b},one{&two,&a};
 Pcb wanted{};wanted.local_port=50001;wanted.remote_port=18444;wanted.local_ip=1;wanted.remote_ip=2;
 wanted.state=4;wanted.rcv_nxt=0xfffffff0;wanted.rcv_wnd=2016;wanted.rcv_ann_wnd=2048;
 wanted.rcv_ann_right_edge=0x7f0;wanted.lastack=123;wanted.snd_nxt=124;
 wanted.ooseq=&one;wanted.refused_data=&refused;wanted.flags=0x100;wanted.nrtx=3;wanted.rtime=-1;
 Pcb foreign=wanted;foreign.remote_ip=3;foreign.next=&wanted;
 const Pcb *heads[]={nullptr,&foreign,nullptr};
 bridge::TcpFlowDiagnostic d;d.values[15]=17;d.values[16]=1;d.values[17]=1;
 auto match=[](const Pcb &p){return p.local_port==50001&&p.remote_port==18444&&p.local_ip==1&&p.remote_ip==2;};
 const auto saved=wanted;
 bridge::capture_tcp_flow(heads,3,match,d);
 const unsigned expected[]={1,1,50001,4,0xfffffff0,2016,2048,0x7f0,123,124,
 #if TCP_QUEUE_OOSEQ
 1200,
 #else
 0,
 #endif
 333,0x100,3,0xffffffff,17,1,1};
 assert(!memcmp(d.values,expected,sizeof(expected)));
 assert(!memcmp(&saved,&wanted,sizeof(wanted)) && one.next==&two && two.next==nullptr);
 // Repeat does not accumulate stale queue bytes; a missing match clears PCB fields.
 bridge::capture_tcp_flow(heads,3,match,d);assert(!memcmp(d.values,expected,sizeof(expected)));
 wanted.remote_port=18445;
 bridge::capture_tcp_flow(heads,3,match,d);
 assert(d.values[0]==1&&d.values[15]==17&&d.values[17]==1);
 for(unsigned i=1;i<15;++i)assert(d.values[i]==0);
}
'''
with tempfile.TemporaryDirectory() as name:
 p=Path(name);(p/'lwip').mkdir();(p/'test.cpp').write_text(SOURCE)
 for queue in (0,1):
  (p/'lwip/opt.h').write_text(f'#define TCP_QUEUE_OOSEQ {queue}\n')
  subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=undefined','-DBL616CL_TLS_FLOW_DIAGNOSTIC=1','-I'+str(p),'-I'+str(ROOT/'UNOR4BL616CLBridge'),str(p/'test.cpp'),'-o',str(p/'test')],check=True)
  subprocess.run([str(p/'test')],check=True)
print('BL616CL_TCP_FLOW_DIAGNOSTIC_PASS')
