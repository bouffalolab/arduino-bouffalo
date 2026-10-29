"""Execute production locked snapshot and real AT dispatch with native fixtures."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
text = (ROOT/'UNOR4BL616CLBridge/bouffalo_tcp_client_backend.cpp').read_text()
start = text.index('bool get_rx_budget_diagnostic(')
end = text.index('\n#endif\n\n} // namespace', start)
source = r'''
#include "tcp_client_service.h"
#include <cassert>
#include <cstring>
#include <string>
#define LWIP_STATS 1
#define MEM_STATS 1
#define MEMP_STATS 1
#define TCP_QUEUE_OOSEQ 1
#define MEMP_NUM_TCP_PCB 32
#define MEMP_TCP_PCB 0
#define MEM_SIZE 40960
#define ESTABLISHED 4
struct pbuf { pbuf *next; unsigned tot_len,ref; };
struct tcp_seg { tcp_seg *next; pbuf *p; };
struct tcp_pcb { tcp_pcb *next; unsigned rcv_nxt,rcv_wnd,rcv_ann_right_edge,state; tcp_seg *ooseq; pbuf *refused_data; };
static tcp_pcb *tcp_active_pcbs,*tcp_bound_pcbs,*tcp_tw_pcbs;
struct Stats { unsigned used,max,err; } native_mem={4000,12000,0}, pool={5,0,0};
static struct { Stats mem;Stats *memp[1]; } lwip_stats={native_mem,{&pool}};
static unsigned locks,unlocks,depth;
static bool fair_healthy=true;
bool tcp_rx_native_policy_healthy() { return fair_healthy; }
#define LOCK_TCPIP_CORE() do { assert(depth==0);++locks;++depth; } while(0)
#define UNLOCK_TCPIP_CORE() do { assert(depth==1);++unlocks;--depth; } while(0)
#define TCP_WND_MAX(p) ((void)(p),2048U)
static unsigned pbuf_clen(const pbuf *p) { unsigned n=0;for(;p;p=p->next)++n;return n; }
using bridge::TcpRxBudgetDiagnostic;
''' + text[start:end] + r'''
static std::string output;
static size_t write(void*,const uint8_t *data,size_t n) { assert(!depth);output.append((const char*)data,n);return n; }
static bridge::at::CommandResult dispatch(void *p,const bridge::at::Command &c,bridge::at::AtTransport &t) {
 return static_cast<bridge::TcpClientService*>(p)->handle_command(c,t);
}
int main() {
 using D=TcpRxBudgetDiagnostic;
 assert(!get_rx_budget_diagnostic(nullptr,nullptr) && locks==0);
 D d;fair_healthy=false;assert(!get_rx_budget_diagnostic(nullptr,&d));
 assert(!depth && locks==unlocks);fair_healthy=true;assert(get_rx_budget_diagnostic(nullptr,&d));
 assert(d.values[D::Active]==0 && d.values[D::MinAccept]==0 && locks==unlocks && !depth);
 tcp_pcb a={},b={},bound={},tw={};pbuf tail={nullptr,100,2},head={&tail,400,1};
 tcp_seg seg={nullptr,&head};
 a.next=&b;a.rcv_nxt=0xffffff00U;a.rcv_wnd=512;a.rcv_ann_right_edge=a.rcv_nxt+512;
 a.state=ESTABLISHED;a.ooseq=&seg;a.refused_data=&tail;
 b.rcv_nxt=2000;b.rcv_wnd=0;b.rcv_ann_right_edge=1999;b.state=7;
 tcp_active_pcbs=&a;tcp_bound_pcbs=&bound;tcp_tw_pcbs=&tw;
 const auto before=a;const auto saved_head=head;const auto saved_tail=tail;
 assert(get_rx_budget_diagnostic(nullptr,&d));
 assert(!memcmp(&a,&before,sizeof(a)) && !memcmp(&head,&saved_head,sizeof(head)) &&
        !memcmp(&tail,&saved_tail,sizeof(tail)));
 assert(d.values[D::PoolUsed]==5 && d.values[D::Active]==2 && d.values[D::Established]==1);
 assert(d.values[D::OtherActive]==1 && d.values[D::Bound]==1 && d.values[D::TimeWait]==1);
 assert(d.values[D::CapTotal]==4096 && d.values[D::AcceptTotal]==512 && d.values[D::WindowDebitTotal]==3584);
 assert(d.values[D::AnnouncedOutstandingTotal]==512 && d.values[D::AnnouncedOverAccept]==0);
 assert(d.values[D::MinAccept]==0 && d.values[D::MaxAccept]==512 && d.values[D::ZeroWindows]==1);
 assert(d.values[D::OoseqPbufs]==2 && d.values[D::OoseqBytes]==400);
 assert(d.values[D::RefusedPbufs]==1 && d.values[D::RefusedBytes]==100);
 // Detect shrinking acceptance behind an outstanding announced edge.
 a.rcv_wnd=511;assert(get_rx_budget_diagnostic(nullptr,&d));assert(d.values[D::AnnouncedOverAccept]==1);
 a.rcv_wnd=2049;assert(!get_rx_budget_diagnostic(nullptr,&d));assert(!depth && locks==unlocks);
 a.rcv_wnd=512;
 bridge::TcpClientBackend backend={};
#if BL616CL_TCP_RX_BUDGET_DIAGNOSTIC
 backend.get_rx_budget_diagnostic=get_rx_budget_diagnostic;
#endif
 bridge::TcpClientService service(backend);
 bridge::at::AtTransport transport({256,64,512,20},write,nullptr);transport.set_command_handler(dispatch,&service);
 auto command=[&](const char *s,bool ok=true) {
  output.clear();transport.feed((const uint8_t*)s,strlen(s),1);
  while(transport.output_pending())transport.flush_output();
  assert(output.find(ok?"OK\r\n":"ERROR\r\n")!=std::string::npos);
 };
 command("AT+TCPRXBUDGET?\r\n");
#if BL616CL_TCP_RX_BUDGET_DIAGNOSTIC
 assert(output=="+TCPRXBUDGET: 1,1,32,5,2,1,1,1,1,4096,512,3584,512,0,0,512,512,1,1,2,400,1,100,40960,4000,12000,0\r\nOK\r\n");
#else
 assert(output=="+TCPRXBUDGET: 1,0\r\nOK\r\n");
#endif
 const auto called=locks;
 command("AT+TCPRXBUDGET=1\r\n",false);command("AT+TCPRXBUDGET\r\n",false);
 assert(locks==called);
#if BL616CL_TCP_RX_BUDGET_DIAGNOSTIC
 backend.get_rx_budget_diagnostic=nullptr;bridge::TcpClientService missing(backend);
 transport.set_command_handler(dispatch,&missing);command("AT+TCPRXBUDGET?\r\n",false);
#endif
 assert(!depth && locks==unlocks);
}
'''
with tempfile.TemporaryDirectory() as directory:
    work=Path(directory);(work/'test.cpp').write_text(source)
    for enabled in (0,1):
        binary=work/'test'
        subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-DBL616CL_TCP_RX_FAIR_WINDOW=1',f'-DBL616CL_TCP_RX_BUDGET_DIAGNOSTIC={enabled}',
                        '-I'+str(ROOT/'UNOR4BL616CLBridge'),str(work/'test.cpp'),
                        str(ROOT/'UNOR4BL616CLBridge/at_transport.cpp'),
                        str(ROOT/'UNOR4BL616CLBridge/tcp_client_service.cpp'),
                        str(ROOT/'UNOR4BL616CLBridge/tcp_runtime_diagnostic.cpp'),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
print('BL616CL_TCP_RX_BUDGET_DIAGNOSTIC_PASS')
