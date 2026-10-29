"""Exercise production input ownership with chained packets and allocation failure."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]
class StaRxOwnershipTest(unittest.TestCase):
    def test_ownership(self):
        with tempfile.TemporaryDirectory() as directory:
            temp = Path(directory)
            (temp/'lwip').mkdir(); (temp/'sdk/mm').mkdir(parents=True)
            (temp/'lwip/opt.h').write_text('#define MEM_SIZE 24576\n#define MEM_ALIGNMENT 4\n')
            (temp/'lwip/stats.h').write_text('extern struct test_stats { struct { unsigned used; } mem; } lwip_stats;\n')
            (temp/'lwip/pbuf.h').write_text('''#include <stdint.h>
struct pbuf { uint16_t tot_len, len; uint8_t *payload; struct pbuf *next; unsigned freed; };
#define PBUF_RAW 0
#define PBUF_RAM 1
uint16_t pbuf_copy_partial(const struct pbuf*,void*,uint16_t,uint16_t);
struct pbuf *pbuf_clone(int,int,struct pbuf*);
uint8_t pbuf_free(struct pbuf*);
''')
            (temp/'sdk/mm/mm.h').write_text('''#include <stdint.h>
extern unsigned locks;
static inline uintptr_t mm_lock_save(void) { ++locks; return 123; }
static inline void mm_unlock_restore(uintptr_t f) { if(f!=123 || !locks) __builtin_trap(); --locks; }
''')
            main=temp/'main.cpp'
            main.write_text('''#include <cassert>
#include <cstring>
#include "sta_rx_ownership.h"
extern "C" {
#include "lwip/pbuf.h"
}
extern "C" unsigned bridge_tcp_ooseq_pbuf_limit(void);
#if BL616CL_STA_TCP_RX_RESERVE_BYTES
extern "C" {
#include "lwip/opt.h"
#include "lwip/stats.h"
test_stats lwip_stats;
}
bool race;
#endif
unsigned locks, clones, frees; bool fail;
uint8_t output[256]; pbuf copy;
extern "C" uint16_t pbuf_copy_partial(const pbuf *p,void *dest,uint16_t n,uint16_t off) {
 unsigned done=0; auto *out=static_cast<uint8_t*>(dest);
 for(;p && done<n;p=p->next) {
  if(off>=p->len) { off-=p->len; continue; }
  unsigned take=p->len-off; if(take>n-done)take=n-done;
  memcpy(out+done,p->payload+off,take); done+=take;off=0;
 } return done;
}
extern "C" pbuf *pbuf_clone(int layer,int type,pbuf *p) {
 assert(!locks && layer==PBUF_RAW && type==PBUF_RAM); ++clones;
 if(fail)return nullptr;
 assert(pbuf_copy_partial(p,output,p->tot_len,0)==p->tot_len);
 #if BL616CL_STA_TCP_RX_RESERVE_BYTES
 lwip_stats.mem.used += race ? 200U : 100U;
#endif
 copy={p->tot_len,p->tot_len,output,nullptr,0};return &copy;
}
extern "C" uint8_t pbuf_free(pbuf *p) { assert(!locks); assert(!p->freed);++p->freed;++frees;
#if BL616CL_STA_TCP_RX_RESERVE_BYTES
 if(p==&copy)lwip_stats.mem.used-=100U;
#endif
 return 1; }
unsigned get(const uint8_t *r,unsigned off) { return r[off]|unsigned(r[off+1])<<8|unsigned(r[off+2])<<16|unsigned(r[off+3])<<24; }
int main() {
 uint8_t bytes[100];for(unsigned i=0;i<100;++i)bytes[i]=i;
 bytes[12]=8;bytes[13]=0;bytes[14]=0x46;bytes[23]=6;
 pbuf tail={84,84,bytes+16,nullptr,0}, p={100,16,bytes,&tail,0};
#if BL616CL_AP_TCP_OWNED_RX
 assert(bridge::sta_rx_detach(false,&p)==&copy && clones==1 && frees==1);
 assert(!memcmp(output,bytes,100) && p.freed==1);
 p.freed=0; clones=frees=0;
#else
 assert(bridge::sta_rx_detach(false,&p)==&p && !clones && !frees);
#endif
 bytes[20]=0;bytes[21]=0;bytes[23]=17; assert(bridge::sta_rx_detach(true,&p)==&p && !clones);
 bytes[23]=6; assert(bridge::sta_rx_detach(true,&p)==&copy);
 assert(clones==1 && frees==1 && p.freed==1 && !memcmp(output,bytes,100));
 assert(copy.payload!=p.payload && !locks);
 p.freed=0; fail=true; assert(bridge::sta_rx_detach(true,&p)==nullptr);
 assert(clones==2 && frees==2 && p.freed==1);
 pbuf shortp={10,10,bytes,nullptr,0}; assert(bridge::sta_rx_detach(true,&shortp)==&shortp);
 assert(bridge::sta_rx_detach(true,nullptr)==nullptr);
 assert(bridge_tcp_ooseq_pbuf_limit()==4);
 uint8_t report[64]={};bridge::sta_rx_ownership_report(report);
 assert(report[5]==51 && report[6]==1 && report[7]==4 && get(report,8)==1+BL616CL_AP_TCP_OWNED_RX && get(report,12)==1 && get(report,16)==100U*(1+BL616CL_AP_TCP_OWNED_RX) && !locks);
#if BL616CL_STA_TCP_RX_RESERVE_BYTES
 const unsigned limit=MEM_SIZE-BL616CL_STA_TCP_RX_RESERVE_BYTES;
 fail=false;lwip_stats.mem.used=limit;p.freed=0;
 unsigned before=clones;
 assert(bridge::sta_rx_detach(true,&p)==nullptr && clones==before && p.freed==1);
 // Noninitial IP fragments are payload even if the following bytes resemble ACK.
 bytes[14]=0x45;bytes[16]=0;bytes[17]=40;bytes[20]=0;bytes[21]=1;bytes[46]=0x50;
 p.freed=0;assert(bridge::sta_rx_detach(true,&p)==nullptr && clones==before);
 // A valid pure ACK bypasses the payload floor; scarce radio RX is still freed.
 bytes[21]=0;bytes[47]=0x10;p.tot_len=54;p.freed=0;
 assert(bridge::sta_rx_detach(true,&p)==&copy && clones==before+1 && p.freed==1);
 pbuf_free(&copy);
 // A data packet that fits the estimate but races another allocation is rolled back.
 bytes[17]=80;p.tot_len=100;lwip_stats.mem.used=limit-180;race=true;p.freed=0;
 assert(bridge::sta_rx_detach(true,&p)==nullptr && copy.freed==1 && p.freed==1);
 bridge::sta_rx_ownership_report(report);
 assert(get(report,24)==BL616CL_STA_TCP_RX_RESERVE_BYTES && get(report,28)==3);
 assert(get(report,12)==1 && !locks); // Reserve drops are not allocator failures.
#endif
 // UDP reassembly must release each radio buffer, including noninitial
 // fragments; DF-only and ordinary UDP stay untouched. Copy failure drops
 // and frees the original exactly once.
 fail=false;p.freed=0;p.tot_len=100;bytes[12]=8;bytes[13]=0;
 bytes[14]=0x45;bytes[23]=17;bytes[20]=0;bytes[21]=0;
#if BL616CL_STA_TCP_RX_RESERVE_BYTES
 lwip_stats.mem.used=0;race=false;
#endif
 unsigned udp_before=clones;
 assert(bridge::sta_rx_detach(true,&p)==&p && clones==udp_before && !p.freed);
 bytes[20]=0x40;
 assert(bridge::sta_rx_detach(true,&p)==&p && !p.freed);
 for(unsigned fragment=0;fragment<12;++fragment) {
  bytes[20]=fragment==0 ? 0x20 : 0;bytes[21]=fragment;
  p.freed=0;
  assert(bridge::sta_rx_detach(true,&p)==&copy && p.freed==1);
  assert(!memcmp(output,bytes,100));
#if BL616CL_STA_TCP_RX_RESERVE_BYTES
  lwip_stats.mem.used=0;
#endif
 }
 fail=true;p.freed=0;
 assert(bridge::sta_rx_detach(true,&p)==nullptr && p.freed==1);
 assert(clones==udp_before+13);
 uint8_t fragments[64]={};bridge::udp_rx_fragment_report(fragments);
 assert(fragments[5]==81 && get(fragments,8)==13 && get(fragments,12)==12 && get(fragments,16)==1);
 assert(fragments[22]==13 && (fragments[25]&0xc0)==0x40);
 // Owner mode detaches all IPv6, including extension/fragment-bearing frames,
 // before native TCP matching; default mode keeps the established path.
 fail=false;p.freed=0;p.tot_len=100;bytes[12]=0x86;bytes[13]=0xdd;bytes[14]=0x60;
#if BL616CL_STA_TCP_RX_RESERVE_BYTES
 lwip_stats.mem.used=0;race=false;
#endif
 const unsigned before_ipv6=clones;
#if BL616CL_TCP_RX_MEMORY_OWNER
 assert(bridge::sta_rx_detach(true,&p)==&copy && clones==before_ipv6+1 && p.freed==1);
 assert(!memcmp(output,bytes,100));
#if BL616CL_STA_TCP_RX_RESERVE_BYTES
 // Valid IPv6 TCP ACKs, also after a destination extension, retain the
 // control exemption at a full data floor. Noninitial fragments do not.
 bytes[18]=0;bytes[19]=20;bytes[20]=6;bytes[66]=0x50;p.tot_len=74;p.freed=0;
 lwip_stats.mem.used=MEM_SIZE-BL616CL_STA_TCP_RX_RESERVE_BYTES;
 assert(bridge::sta_rx_detach(true,&p)==&copy && p.freed==1);
 bytes[19]=28;bytes[20]=60;bytes[54]=6;bytes[55]=0;bytes[74]=0x50;
 p.tot_len=82;p.freed=0;lwip_stats.mem.used=MEM_SIZE-BL616CL_STA_TCP_RX_RESERVE_BYTES;
 assert(bridge::sta_rx_detach(true,&p)==&copy && p.freed==1);
 bytes[20]=44;bytes[56]=0;bytes[57]=8;p.freed=0;
 lwip_stats.mem.used=MEM_SIZE-BL616CL_STA_TCP_RX_RESERVE_BYTES;
 assert(bridge::sta_rx_detach(true,&p)==nullptr && p.freed==1);
#endif
#else
 assert(bridge::sta_rx_detach(true,&p)==&p && clones==before_ipv6 && !p.freed);
#endif
}
''')
            binary=temp/'test'
            for reserve,ap,owner in ((r,a,o) for r in (0,4096) for a in (0,1) for o in (0,1)):
                subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-DBL616CL_STA_TCP_OWNED_RX=1','-DBL616CL_AP_TCP_OWNED_RX='+str(ap),'-DBL616CL_TCP_OOSEQ_PBUF_LIMIT=4',
                    '-DBL616CL_STA_TCP_RX_RESERVE_BYTES='+str(reserve),'-DBL616CL_TCP_RX_MEMORY_OWNER='+str(owner),'-DBL616CL_TCP_RX_FAIR_WINDOW='+str(owner),'-I'+str(temp),'-I'+str(ROOT/'UNOR4BL616CLBridge'),
                    str(main),str(ROOT/'UNOR4BL616CLBridge/sta_rx_ownership.cpp'),'-o',str(binary)],check=True)
                subprocess.run([str(binary)],check=True)
    def test_policy_weak_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            temp=Path(directory)
            source=temp/'policy.cpp'
            source.write_text('''#include <cassert>
using u16_t=unsigned short;
#include "lwipopts_user.h"
#ifdef APP_LIMIT
extern "C" unsigned bridge_tcp_ooseq_pbuf_limit(void) { return 4; }
#endif
int main() {
#ifdef APP_LIMIT
 assert(TCP_OOSEQ_PBUFS_LIMIT(nullptr)==4);
#else
 assert(TCP_OOSEQ_PBUFS_LIMIT(nullptr)==65535);
#endif
}
''')
            for flags in ([], ['-DAPP_LIMIT']):
                subprocess.run(['g++','-std=c++11','-I'+str(ROOT/'tools/bridge_runtime/profiles/bridge'),*flags,str(source),'-o',str(temp/'policy')],check=True)
                subprocess.run([str(temp/'policy')],check=True)
            obj=temp/'off.o'
            subprocess.run(['g++','-std=c++11','-c',str(ROOT/'UNOR4BL616CLBridge/sta_rx_ownership.cpp'),'-o',str(obj)],check=True)
            symbols=subprocess.check_output(['nm','-C',str(obj)],text=True)
            for name in ('bridge_tcp_ooseq_pbuf_limit','copied','failed','pbuf_clone',
                         'reserve_dropped','rx_copy_has_room','tcp_has_payload','lwip_stats','clone_in_chunks'):
                self.assertNotIn(name,symbols)

    def test_native_lwip_ooseq_limit(self):
        source=(ROOT/'third_party/bouffalo_sdk/components/net/lwip/lwip/src/core/tcp_in.c').read_text()
        start=source.index('#if defined(TCP_OOSEQ_BYTES_LIMIT) || defined(TCP_OOSEQ_PBUFS_LIMIT)\n        {')
        end=source.index('#endif /* TCP_OOSEQ_BYTES_LIMIT || TCP_OOSEQ_PBUFS_LIMIT */',start)
        block=source[start:end]+'#endif\n'
        with tempfile.TemporaryDirectory() as directory:
            temp=Path(directory)
            code=temp/'limit.cpp'
            code.write_text('''#include <cassert>
#include <cstddef>
using u16_t=unsigned short;
#define LWIP_TCP_SACK_OUT 0
static unsigned budget=4;
#define TCP_OOSEQ_PBUFS_LIMIT(pcb) budget
struct pbuf { unsigned count; };
struct tcp_seg { tcp_seg *next; pbuf *p; unsigned released; };
struct tcp_pcb { tcp_seg *ooseq; };
unsigned pbuf_clen(pbuf *p) { return p->count; }
void tcp_segs_free(tcp_seg *p) { for(;p;p=p->next)++p->released; }
void limit(tcp_pcb *pcb) {
'''+block+'''}
int main() {
 pbuf ps[]={{1},{2},{1},{1},{1}};
 tcp_seg seg[5];for(unsigned i=0;i<5;++i)seg[i]={i<4?&seg[i+1]:nullptr,&ps[i],0};
 tcp_pcb pcb={seg}; limit(&pcb);
 assert(pcb.ooseq==seg && seg[2].next==nullptr);
 assert(!seg[0].released && !seg[1].released && !seg[2].released);
 assert(seg[3].released==1 && seg[4].released==1);
 budget=1; ps[0].count=2; limit(&pcb); assert(!pcb.ooseq);
 for(unsigned i=0;i<5;++i)seg[i]={i<4?&seg[i+1]:nullptr,&ps[i],0};
 budget=65535;pcb.ooseq=seg;limit(&pcb);assert(seg[4].released==0 && seg[3].next==&seg[4]);
}
''')
            subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(code),'-o',str(temp/'limit')],check=True)
            subprocess.run([str(temp/'limit')],check=True)

class GlobalOoseqTest(unittest.TestCase):
    def test_multiple_pcbs_native_trimming(self):
        source=(ROOT/'third_party/bouffalo_sdk/components/net/lwip/lwip/src/core/tcp_in.c').read_text()
        start=source.index('#if defined(TCP_OOSEQ_BYTES_LIMIT) || defined(TCP_OOSEQ_PBUFS_LIMIT)\n        {')
        end=source.index('#endif /* TCP_OOSEQ_BYTES_LIMIT || TCP_OOSEQ_PBUFS_LIMIT */',start)
        block=source[start:end]+'#endif\n'
        with tempfile.TemporaryDirectory() as directory:
            temp=Path(directory)
            (temp/'lwip/priv').mkdir(parents=True)
            (temp/'lwip/priv/tcp_priv.h').write_text("""#pragma once
struct pbuf { unsigned count; };
struct tcp_seg { tcp_seg *next; pbuf *p; unsigned released; };
struct tcp_pcb { tcp_pcb *next; tcp_seg *ooseq; };
extern tcp_pcb *tcp_active_pcbs;
unsigned pbuf_clen(const pbuf *);
""")
            code=temp/'global.cpp'
            code.write_text("""#include <cassert>
#include <cstddef>
#include "sta_rx_ownership.h"
extern "C" {
#include "lwip/priv/tcp_priv.h"
unsigned bridge_tcp_ooseq_pbuf_limit(void);
tcp_pcb *tcp_active_pcbs;
unsigned pbuf_clen(const pbuf *p) { return p->count; }
}
using u16_t=unsigned short;
#define LWIP_TCP_SACK_OUT 0
#define TCP_OOSEQ_PBUFS_LIMIT(pcb) bridge_tcp_ooseq_pbuf_limit()
unsigned freed;
void tcp_segs_free(tcp_seg *p) { for(;p;p=p->next) { assert(!p->released); ++p->released; freed+=p->p->count; } }
void limit(tcp_pcb *pcb) {
"""+block+"""}
unsigned live() {
 unsigned count=0;
 for(auto *pcb=tcp_active_pcbs;pcb;pcb=pcb->next)
  for(auto *s=pcb->ooseq;s;s=s->next) { assert(!s->released); count+=s->p->count; }
 return count;
}
int main() {
 tcp_pcb pcbs[8]={}; pbuf buffers[96]={}; tcp_seg segments[96]={};
 for(unsigned i=0;i<8;++i) pcbs[i].next=i<7?&pcbs[i+1]:nullptr;
 tcp_active_pcbs=pcbs;
 unsigned allocated=0;
 for(unsigned i=0;i<96;++i) {
  auto *pcb=&pcbs[i%8];
  // Include multi-pbuf segments and more than four inserts per PCB.
  buffers[i].count=i<32?1:2; allocated+=buffers[i].count;
  segments[i]={nullptr,&buffers[i],0};
  auto **tail=&pcb->ooseq; while(*tail)tail=&(*tail)->next; *tail=&segments[i];
  limit(pcb);
  unsigned local=0;for(auto *s=pcb->ooseq;s;s=s->next)local+=s->p->count;
  assert(local<=4 && live()+freed==allocated);
#if BL616CL_TCP_OOSEQ_GLOBAL_PBUF_LIMIT
  assert(live()<=BL616CL_TCP_OOSEQ_GLOBAL_PBUF_LIMIT);
#else
  if(i==31)assert(live()==32); // Per-PCB four does not mean global four.
#endif
 }
 uint8_t report[64]={};bridge::sta_rx_ownership_report(report);
 assert(report[20]==BL616CL_TCP_OOSEQ_GLOBAL_PBUF_LIMIT && report[21]==0);
 for(auto &pcb:pcbs) { tcp_segs_free(pcb.ooseq);pcb.ooseq=nullptr; }
 assert(!live() && freed==allocated);
}
""")
            for budget in (0,8):
                binary=temp/('global'+str(budget))
                subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-DBL616CL_TCP_OOSEQ_PBUF_LIMIT=4','-DBL616CL_TCP_OOSEQ_GLOBAL_PBUF_LIMIT='+str(budget),
                    '-I'+str(temp),'-I'+str(ROOT/'UNOR4BL616CLBridge'),str(code),
                    str(ROOT/'UNOR4BL616CLBridge/sta_rx_ownership.cpp'),'-o',str(binary)],check=True)
                subprocess.run([str(binary)],check=True)

if __name__ == '__main__':unittest.main()
