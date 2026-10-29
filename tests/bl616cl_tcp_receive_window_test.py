"""Execute patched native lwIP window accounting, including zero-window reopen."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
SDK=ROOT/'third_party/bouffalo_sdk'
PATCHES=ROOT/'tools/bridge_runtime/profiles/bridge/fhost_patches'
def function(text, signature):
    start=text.index(signature)
    brace=text.index('{',start); level=1; end=brace+1
    while level:
        level+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]
class WindowTest(unittest.TestCase):
    def test_native_accounting(self):
        with tempfile.TemporaryDirectory() as d:
            temp=Path(d)
            for relative in ['core/tcp.c','core/tcp_in.c','core/tcp_out.c','include/lwip/tcp.h']:
                p=temp/'components/net/lwip/lwip/src'/relative
                p.parent.mkdir(parents=True,exist_ok=True)
                p.write_bytes((SDK/'components/net/lwip/lwip/src'/relative).read_bytes())
            for patch in ['0070-bridge-tcp-listen-ingress-diagnostic.patch','0075-bridge-tcp-receive-window-diagnostic.patch','0080-bridge-tcp-rx-native-policy.patch','0081-bridge-tcp-rx-small-grant-announcement.patch','0082-bridge-tcp-rx-fast-timer-poll.patch']:
                subprocess.run(['patch','-s','-p1','-i',str(PATCHES/patch)],cwd=temp,check=True)
            tcp=(temp/'components/net/lwip/lwip/src/core/tcp.c').read_text()
            header=(temp/'components/net/lwip/lwip/src/include/lwip/tcp.h').read_text()
            helper=header[header.index('/* Diagnostic fixed receive cap.'):header.index('#if LWIP_WND_SCALE\n#define RCV_WND_SCALE')]
            macros=header[header.index('#if LWIP_WND_SCALE\n#define RCV_WND_SCALE'):header.index('/* Increments a tcpwnd_size_t')]
            harness=r'''#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef uint32_t tcpwnd_size_t; typedef uint32_t u32_t; typedef uint16_t u16_t;
#define TCP_WND 35040U
#define TCP_MSS 1460U
#define TCP_WND_UPDATE_THRESHOLD 8760U
#define TF_WND_SCALE 1
#define LISTEN 10
#define LWIP_MIN(a,b) ((a)<(b)?(a):(b))
#define LWIP_MAX(a,b) ((a)>(b)?(a):(b))
#define LWIP_ASSERT(m,c) assert(c)
#define LWIP_ASSERT_CORE_LOCKED()
#define LWIP_ERROR(m,c,h) do { if(!(c)) {h;} } while(0)
#define LWIP_DEBUGF(a,b)
#define TCP_SEQ_GEQ(a,b) ((int32_t)((a)-(b))>=0)
#define TCP_SEQ_GT(a,b) ((int32_t)((a)-(b))>0)
extern unsigned bridge_tcp_receive_window(void) __attribute__((weak));
struct tcp_pcb { u32_t rcv_nxt,rcv_wnd,rcv_ann_wnd,rcv_ann_right_edge; unsigned mss,flags,state; };
unsigned acks;
void tcp_ack_now(struct tcp_pcb *p) { (void)p;++acks; }
void tcp_output(struct tcp_pcb *p) { p->rcv_ann_right_edge=p->rcv_nxt+p->rcv_ann_wnd; }
'''+helper+'\n#define BRIDGE_TCP_RECEIVE_WINDOW() bridge_tcp_receive_window_limit()\n'+macros+'\nu32_t\n'+function(tcp,'tcp_update_rcv_ann_wnd(struct tcp_pcb *pcb)')+'\nvoid\n'+function(tcp,'tcp_recved(struct tcp_pcb *pcb, u16_t len)')+r'''
int main(void) {
 unsigned expected=TEST_CAP?TEST_CAP:35040U;
 assert(BRIDGE_TCP_RECEIVE_WINDOW()==expected);
 struct tcp_pcb flows[24]={0};
 for(unsigned i=0;i<24;++i) {
  struct tcp_pcb *p=&flows[i]; p->mss=1460;p->flags=i&1;
  assert(TCP_WND_MAX(p)==expected);
  p->rcv_nxt=0xfffffff0U;p->rcv_ann_right_edge=p->rcv_nxt;
  // All receive credit consumed; application drains without another peer packet.
  p->rcv_wnd=0;p->rcv_ann_wnd=0;
  unsigned before=acks;
  tcp_recved(p,(u16_t)expected);
  assert(p->rcv_wnd==expected && p->rcv_ann_wnd==expected && acks>before);
  assert(p->rcv_ann_right_edge==p->rcv_nxt+expected);
  // Duplicate/excess credit cannot inflate the receive limit.
  tcp_recved(p,65535);assert(p->rcv_wnd==expected);
  for(unsigned j=0;j<200;++j) {
   unsigned n=37U+(j%400);p->rcv_wnd-=n;p->rcv_nxt+=n;
   tcp_recved(p,(u16_t)n); assert(p->rcv_wnd==expected && p->rcv_ann_wnd<=expected);
  }
 }
}
'''
            source=temp/'test.c';source.write_text(harness)
            for cap in [0,1460,2920,35040]:
                for scale in [0,1]:
                    args=['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',f'-DTEST_CAP={cap}',f'-DLWIP_WND_SCALE={scale}',str(source)]
                    if cap:
                        callback=temp/'callback.c';callback.write_text(f'unsigned bridge_tcp_receive_window(void) {{ return {cap}; }}\n');args.append(str(callback))
                    binary=temp/'test';subprocess.run(args+['-o',str(binary)],check=True);subprocess.run([str(binary)],check=True)
            # Compile and link actual bridge callback; default build exports no policy.
            app=ROOT/'UNOR4BL616CLBridge/tcp_receive_window.cpp'
            for cap in [0,1460]:
                obj=temp/'policy.o';subprocess.run(['g++','-c',f'-DBL616CL_TCP_RECEIVE_WINDOW_BYTES={cap}',str(app),'-o',str(obj)],check=True)
                symbols=subprocess.check_output(['nm',str(obj)],text=True)
                assert ('bridge_tcp_receive_window' in symbols)==bool(cap)
if __name__=='__main__':unittest.main()
