"""Execute native allocation/free/window accounting with the production policy."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from bl616cl_tcp_receive_window_test import function
ROOT=Path(__file__).resolve().parents[1]
PATCHES=ROOT/'tools/bridge_runtime/profiles/bridge/fhost_patches'

class NativePolicyTest(unittest.TestCase):
    def test_default_exports_no_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            obj=Path(directory)/'policy.o'
            subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-c',
                str(ROOT/'UNOR4BL616CLBridge/tcp_rx_native_policy.cpp'),'-o',str(obj)],check=True)
            symbols=subprocess.check_output(['nm',str(obj)],text=True)
            self.assertNotIn('bridge_tcp_rx_policy',symbols)

    def test_production_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            temp=Path(directory)
            for rel in ('core/tcp.c','core/tcp_in.c','core/tcp_out.c','core/mem.c','core/pbuf.c','include/lwip/tcp.h'):
                p=temp/'components/net/lwip/lwip/src'/rel;p.parent.mkdir(parents=True,exist_ok=True)
                p.write_bytes((ROOT/'third_party/bouffalo_sdk/components/net/lwip/lwip/src'/rel).read_bytes())
            for name in ('0070-bridge-tcp-listen-ingress-diagnostic.patch','0075-bridge-tcp-receive-window-diagnostic.patch','0076-bridge-lwip-allocation-failure-diagnostic.patch','0077-bridge-tcp-tx-preallocation-diagnostic.patch','0078-bridge-lwip-atomic-rx-reserve.patch','0079-bridge-lwip-memory-lifecycle.patch','0080-bridge-tcp-rx-native-policy.patch','0081-bridge-tcp-rx-small-grant-announcement.patch','0082-bridge-tcp-rx-fast-timer-poll.patch','0083-bridge-tcp-rx-memory-owner.patch'):
                subprocess.run(['patch','-s','-p1','-i',str(PATCHES/name)],cwd=temp,check=True)
            tcp=(temp/'components/net/lwip/lwip/src/core/tcp.c').read_text()
            self.assertNotIn('bridge_tcp_rx_policy.poll()', function(tcp, 'tcp_setprio(struct tcp_pcb *pcb, u8_t prio)'))
            incoming=(temp/'components/net/lwip/lwip/src/core/tcp_in.c').read_text()
            self.assertNotIn('tcp_receive(', function(incoming, 'tcp_timewait_input(struct tcp_pcb *pcb)'))
            header=(temp/'components/net/lwip/lwip/src/include/lwip/tcp.h').read_text()
            prefix=header[header.index('/* Diagnostic fixed receive cap.'):header.index('/* Increments a tcpwnd_size_t')]
            fake=r'''
#pragma once
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef uint32_t tcpwnd_size_t,u32_t; typedef uint16_t u16_t; typedef uint8_t u8_t;
#define TCP_WND 35040U
#define TCP_MSS 1460U
#define TCP_WND_UPDATE_THRESHOLD 8760U
#define TF_WND_SCALE 1
#define TF_ACK_DELAY 2
#define TF_ACK_NOW 4
#define TF_CLOSEPEND 8
#define tcp_clear_flags(p,f) ((p)->flags &= ~(f))
#define TCPWND_MIN16(x) ((x)<65535U?(x):65535U)
#define LWIP_MIN(a,b) ((a)<(b)?(a):(b))
#define LWIP_MAX(a,b) ((a)>(b)?(a):(b))
#define LWIP_ASSERT(m,c) assert(c)
#define LWIP_ASSERT_CORE_LOCKED() assert(core_locked && !irq_locked && !heap_locked)
#define LWIP_ERROR(m,c,h) do { if(!(c)) {h;} } while(0)
#define LWIP_DEBUGF(a,b)
#define TCP_SEQ_GEQ(a,b) ((int32_t)((a)-(b))>=0)
#define TCP_SEQ_GT(a,b) ((int32_t)((a)-(b))>0)
#define CLOSED 0
#define ESTABLISHED 4
#define TIME_WAIT 6
#define LISTEN 10
#define LWIP_TCP_PCB_NUM_EXT_ARGS 0
#define TCP_SND_BUF 35040
#define TCP_TTL 64
#define INITIAL_MSS 536
#define TCP_SLOW_INTERVAL 500
#define TCP_TIMER_PRECISE_NEEDED 0
#define LWIP_CALLBACK_API 0
#define LWIP_TCP_KEEPALIVE 0
#define TCP_KEEPIDLE_DEFAULT 7200000
#define MEMP_TCP_PCB 0
#define LAST_ACK 9
#define CLOSING 8
#define MEMP_STATS_DEC(a,b) ((void)0)
struct tcp_pcb {
 struct tcp_pcb *next; void *refused_data;
 u32_t rcv_nxt,rcv_wnd,rcv_ann_wnd,rcv_ann_right_edge;
 unsigned mss,flags,state,prio,snd_buf,ttl,rto,sv,cwnd,tmr,last_timer,ssthresh,keep_idle,rcv_scale;
 int rtime;
};
extern unsigned core_locked,irq_locked,heap_locked;
extern tcp_pcb *tcp_tw_pcbs;
struct pbuf { struct pbuf *next; void *payload; u16_t tot_len,len; unsigned ref;u8_t type_internal,flags; };
#define PBUF_RAM 128
#define PBUF_FLAG_IS_CUSTOM 2
#if BL616CL_TCP_RX_MEMORY_OWNER
int bridge_tcp_rx_memory_admit(struct tcp_pcb *,struct pbuf *) __attribute__((weak));
void bridge_lwip_mem_lifecycle(void *,unsigned,unsigned);
#endif
void tcp_ack_now(struct tcp_pcb *);
int tcp_output(struct tcp_pcb *);
u32_t tcp_update_rcv_ann_wnd(struct tcp_pcb *);
void tcp_free(struct tcp_pcb *);
'''+prefix
            # Use the SDK's real type flags: PBUF_RAM includes 0x0200, while
            # native pbuf_init_alloced_pbuf stores only its low 8 bits.
            pbuf_header=(ROOT/'third_party/bouffalo_sdk/components/net/lwip/lwip/src/include/lwip/pbuf.h').read_text()
            flags=pbuf_header[pbuf_header.index('/* Base flags for pbuf_type definitions: */'):pbuf_header.index('typedef enum {',pbuf_header.index('/* Base flags for pbuf_type definitions: */'))]
            ram=next(line for line in pbuf_header.splitlines() if '  PBUF_RAM = ' in line).split('=',1)[1].strip().rstrip(',')
            fake=fake.replace('#define PBUF_RAM 128',flags+'\n#define PBUF_RAM '+ram)
            (temp/'sdk/mm').mkdir(parents=True)
            (temp/'sdk/mm/mm.h').write_text('#pragma once\n#include <stdint.h>\nuintptr_t mm_lock_save(void);\nvoid mm_unlock_restore(uintptr_t);\n')
            (temp/'lwip/priv').mkdir(parents=True)
            (temp/'lwip/tcp.h').write_text(fake)
            (temp/'lwip/priv/tcp_priv.h').write_text('#include "lwip/tcp.h"\n')
            source=r'''
#include <string.h>
#include <stdlib.h>
#include "tcp_rx_native_policy.h"
extern "C" {
#include "lwip/tcp.h"
unsigned core_locked=1,irq_locked=0,heap_locked=0,acks=0,outputs=0,frees=0;
uintptr_t mm_lock_save() { assert(!irq_locked);irq_locked=1;return 0; }
void mm_unlock_restore(uintptr_t flags) { assert(irq_locked && !flags);irq_locked=0; }
static unsigned tcp_ticks=1,tcp_timer_ctr=1,tcp_active_pcbs_changed=0;
static tcp_pcb *tcp_active_pcbs=nullptr;
tcp_pcb *tcp_tw_pcbs=nullptr;
static void tcp_close_shutdown_fin(tcp_pcb *) {}
static void tcp_process_refused_data(tcp_pcb *) {}
static tcp_pcb storage[32]; static bool allocated[32];
static void *memp_malloc(unsigned) { for(unsigned i=0;i<32;++i) if(!allocated[i]) {allocated[i]=true;return &storage[i];} return nullptr; }
static void memp_free(unsigned,void *p) { assert(allocated[(tcp_pcb*)p-storage]); allocated[(tcp_pcb*)p-storage]=false;++frees; }
static void tcp_handle_closepend() {}
static void tcp_kill_timewait() {}
static void tcp_kill_state(unsigned) {}
static void tcp_kill_prio(unsigned) {}
void tcp_ack_now(tcp_pcb *) { assert(!irq_locked && !heap_locked);++acks; }
int tcp_output(tcp_pcb *p) { assert(!irq_locked && !heap_locked);++outputs;p->rcv_ann_right_edge=p->rcv_nxt+p->rcv_ann_wnd;return 0; }
'''
            for sig,typ in (('tcp_fasttmr(void)','void'),('tcp_alloc(u8_t prio)','struct tcp_pcb *'),('tcp_free(struct tcp_pcb *pcb)','void'),('tcp_update_rcv_ann_wnd(struct tcp_pcb *pcb)','u32_t'),('tcp_recved(struct tcp_pcb *pcb, u16_t len)','void')):
                source+='\n'+typ+'\n'+function(tcp,sig)
            # Execute the actual FIN return statements, including the separate
            # deferred-refused-data path. Other branch effects are irrelevant.
            start=incoming.index('            if (&bridge_tcp_rx_policy) {')
            fin=function(incoming[start:],'if (&bridge_tcp_rx_policy)')
            # Include its legacy else branch as well using the exact source span.
            end=incoming.index('            TCP_EVENT_CLOSED',start)
            fin=incoming[start:end]
            start=tcp.index('        if (&bridge_tcp_rx_policy) {',tcp.index('tcp_process_refused_data('))
            end=tcp.index('        TCP_EVENT_CLOSED',start)
            deferred=tcp[start:end]
            # Execute the native active-open reset, scale negotiation and the
            # actual SYN/SYN-ACK header window encoding with the registered cap.
            connect=tcp[tcp.index('  iss = tcp_next_iss(pcb);'):tcp.index('  pcb->snd_wnd = TCP_WND;')]
            line=next(line for line in connect.splitlines() if 'rcv_wnd = pcb->rcv_ann_wnd' in line)
            start=incoming.index('            pcb->rcv_scale = TCP_RCV_SCALE;')
            end=incoming.index('          }\n          break;',start)
            scale_block=incoming[start:end]
            outgoing=(temp/'components/net/lwip/lwip/src/core/tcp_out.c').read_text()
            start=outgoing.index('  /* advertise our receive window size in this TCP segment */')
            end=outgoing.index('  /* Add any requested options.',start)
            wire=outgoing[start:end]
            source+='\n}\n#define TCP_RCV_SCALE 2\n#define tcp_set_flags(p,f) ((p)->flags |= (f))\n'
            source+='static void active_reset(tcp_pcb *pcb) {\n'+line+'\n}\n'
            source+='static void scale_syn(tcp_pcb *pcb) {\n'+scale_block+'\n}\n'
            source+='''#define TF_SEG_OPTS_WND_SCALE 1
#define lwip_htons(x) (x)
struct tcp_hdr { unsigned wnd; };
struct tcp_seg { unsigned flags; tcp_hdr *tcphdr; };
static void wire_window(tcp_pcb *pcb, tcp_seg *seg) {
'''+wire+'\n}\n'
            source+='static void immediate_fin(tcp_pcb *pcb) {\n'+fin+'\n}\n'
            source+='static void deferred_fin(tcp_pcb *pcb) {\n'+deferred+'\n}\n'
            native_pbuf=(temp/'components/net/lwip/lwip/src/core/pbuf.c').read_text()
            owner_source=r'''
#if BL616CL_TCP_RX_MEMORY_OWNER
#define LWIP_UNUSED_ARG(x) (void)(x)
#define PERF_START
#define PERF_STOP(m)
#define SYS_ARCH_DECL_PROTECT(x)
#define SYS_ARCH_PROTECT(x)
#define SYS_ARCH_UNPROTECT(x)
#define LWIP_SUPPORT_CUSTOM_PBUF 0
#define LWIP_PBUF_REF_T unsigned
#define MEMP_PBUF_POOL 1
#define MEMP_PBUF 2
#define pbuf_get_allocsrc(p) ((p)->type_internal & 15)
static pbuf *blocks[256]; static unsigned charges[256];
extern "C" unsigned bridge_lwip_mem_charge(const void *p) {
 assert(!irq_locked && !heap_locked);heap_locked=1;
 unsigned result=0;
 for(unsigned i=0;i<256;++i) if(blocks[i]==p) result=charges[i];
 heap_locked=0;return result;
}
static pbuf *make_block(unsigned bytes) {
 assert(!irq_locked && !heap_locked);
 auto *p=(pbuf *)calloc(1,sizeof(pbuf)+bytes);assert(p);
 p->payload=p+1;p->len=p->tot_len=bytes;p->ref=1;p->type_internal=u8_t(PBUF_RAM);
 unsigned i=0;while(i<256 && blocks[i]) ++i;assert(i<256);
 blocks[i]=p;charges[i]=sizeof(*p)+bytes+16;
 heap_locked=1;bridge_lwip_mem_lifecycle(p,0,charges[i]);heap_locked=0;
 return p;
}
static void mem_free(void *p) {
 assert(!irq_locked && !heap_locked);
 unsigned i=0;while(i<256 && blocks[i]!=p) ++i;assert(i<256);
 heap_locked=1;bridge_lwip_mem_lifecycle(p,charges[i],0);heap_locked=0;
 blocks[i]=nullptr;charges[i]=0;free(p);
}
'''
            for sig,typ in (('pbuf_free(struct pbuf *p)','u8_t'),('pbuf_remove_header(struct pbuf *p, size_t header_size_decrement)','u8_t'),('pbuf_free_header(struct pbuf *q, u16_t size)','struct pbuf *')):
                owner_source+='\n'+typ+'\n'+function(native_pbuf,sig)
            # Execute the real native before-process admission branch, including
            # its drop/free/return, so denial cannot acknowledge the new input.
            gate_start=incoming.index('    if (bridge_tcp_rx_memory_admit &&')
            gate=function(incoming[gate_start:],'if (bridge_tcp_rx_memory_admit &&')
            owner_source+='\n#define TCP_FIN 1\nstatic void input_gate(tcp_pcb *pcb,pbuf *p,unsigned flags,bool &processed) {\n'+gate+'\nprocessed=true;\n}\n'
            owner_source+=r'''
static unsigned word(const uint8_t *p) { return unsigned(p[0])|(unsigned(p[1])<<8)|(unsigned(p[2])<<16)|(unsigned(p[3])<<24); }
static unsigned physical() {
#if BL616CL_TCP_RX_PHYSICAL_FAIR
 uint8_t fair[64]={};bridge::tcp_rx_physical_report(fair);
 assert(fair[5]==60 && fair[6]==1);
 assert(word(fair+32)==256);
 assert(word(fair+8)+word(fair+12)<=20480);
 assert(word(fair+16)+word(fair+20)<=128);
 assert(word(fair+24)<=4096 && word(fair+28)<=16 && !word(fair+52));
#endif
 uint8_t report[64]={};bridge::tcp_rx_memory_report(report);assert(report[6]==1 && !word(report+36));return word(report+12); }
static void memory_test() {
 assert(PBUF_RAM != u8_t(PBUF_RAM));
 auto *pcb=tcp_alloc(1);assert(pcb);pcb->state=ESTABLISHED;
 auto *head=make_block(128),*tail=make_block(64);head->next=tail;head->tot_len=192;
 bool processed=false;input_gate(pcb,head,0,processed);assert(processed);
 const unsigned charge=physical();assert(charge>192);
 pcb->rcv_nxt+=192;pcb->rcv_wnd-=192;bridge_tcp_rx_policy.received(pcb,192);
 head->ref=2; // another native consumer retains this allocation after admission
 assert(pbuf_free_header(head,127)==head);tcp_recved(pcb,127);assert(physical()==charge);
 assert(pbuf_free_header(head,1)==tail && head->ref==1);tcp_recved(pcb,1);assert(physical()==charge);
 pcb->state=TIME_WAIT;tcp_tw_pcbs=pcb;tcp_fasttmr();
 uint8_t report[64]={};bridge::tcp_rx_memory_report(report);
 assert(word(report+28)==1 && physical()==charge); // retired owner keeps full native charge
 tcp_recved(pcb,64);assert(physical()==charge && bridge::tcp_rx_native_policy_healthy());
 tcp_tw_pcbs=nullptr;tcp_free(pcb);auto *replacement=tcp_alloc(1);assert(replacement==pcb);
 // Last native references can die on an application task, outside TCP core.
 core_locked=0;assert(pbuf_free(head)==1 && pbuf_free(tail)==1);assert(!physical());core_locked=1;
 assert(replacement->rcv_wnd==256 && bridge::tcp_rx_native_policy_healthy());
 replacement->state=ESTABLISHED;
 // A metadata-exhausted partial chain must be rejected and fully cleaned by
 // the real ingress branch before processing any data or acknowledging it.
 head=nullptr;tail=nullptr;
 for(unsigned i=0;i<129;++i) {auto *p=make_block(1);if(tail)tail->next=p;else head=p;tail=p;}
 head->tot_len=129;processed=false;input_gate(replacement,head,0,processed);
 assert(!processed && !physical() && bridge::tcp_rx_native_policy_healthy());

#if BL616CL_TCP_RX_PHYSICAL_FAIR
 // Hold sixteen real one-byte native pbufs without reading. The next incoming
 // packet is dropped before processing; an independent peer still progresses.
 pbuf *held[16];
 for(unsigned i=0;i<16;++i) {
  held[i]=make_block(1);processed=false;input_gate(replacement,held[i],0,processed);assert(processed);
 }
 head=make_block(1);processed=false;input_gate(replacement,head,0,processed);assert(!processed);
 auto *peer=tcp_alloc(1);assert(peer);peer->state=ESTABLISHED;
 for(unsigned i=0;i<100;++i) {
  auto *small=make_block(256);processed=false;input_gate(peer,small,0,processed);assert(processed);
  assert(pbuf_free(small)==1 && bridge::tcp_rx_native_policy_healthy());
 }
 replacement->state=TIME_WAIT;tcp_tw_pcbs=replacement;tcp_fasttmr();
 tcp_tw_pcbs=nullptr;tcp_free(replacement);replacement=tcp_alloc(1);assert(replacement);
 for(auto *small:held) assert(pbuf_free(small)==1);
 tcp_free(peer);assert(!physical());replacement->state=ESTABLISHED;
#endif
 // An oversized full native charge is denied independently of sequence credit.
 head=make_block(20480);processed=false;input_gate(replacement,head,0,processed);
 assert(!processed && !physical());
 head=make_block(1);head->type_internal=1;processed=false;
 input_gate(replacement,head,0,processed); // fake pool free is not permitted
}
#endif
'''
            # Pool/custom rejection is tested through the production callback;
            # use the real native drop/free only for owned RAM chains.
            owner_source=owner_source.replace('input_gate(replacement,head,0,processed); // fake pool free is not permitted',
                'assert(!bridge_tcp_rx_memory_admit(replacement,head));head->type_internal=u8_t(PBUF_RAM);assert(pbuf_free(head)==1);\n tcp_free(replacement);assert(!physical() && bridge::tcp_rx_native_policy_healthy());')
            source+=owner_source
            source+=r'''
static void receive(tcp_pcb *pcb,unsigned bytes) {
 assert(bytes<=pcb->rcv_wnd);
 pcb->rcv_nxt+=bytes;pcb->rcv_wnd-=bytes;
 bridge_tcp_rx_policy.received(pcb,bytes);
 tcp_update_rcv_ann_wnd(pcb);
}
int main() {
 tcp_pcb *flows[32];
 for(unsigned i=0;i<32;++i) {
  flows[i]=tcp_alloc(1); assert(flows[i] && flows[i]->rcv_wnd==256);
  active_reset(flows[i]); scale_syn(flows[i]);
  tcp_hdr hdr={};tcp_seg syn={TF_SEG_OPTS_WND_SCALE,&hdr};
  wire_window(flows[i],&syn);assert(hdr.wnd==256); // SYN and SYN-ACK are unscaled
  syn.flags=0;wire_window(flows[i],&syn);
  assert(hdr.wnd==(LWIP_WND_SCALE?64:256));
  flows[i]->state=ESTABLISHED;flows[i]->rcv_nxt=0xffffff00U;
  flows[i]->rcv_ann_right_edge=flows[i]->rcv_nxt+256;
 }
 assert(!tcp_alloc(1));
 for(auto *p:flows) {
  receive(p,64);tcp_recved(p,64);
  assert(p->rcv_wnd==256 && TCP_WND_MAX(p)==256); // short messages borrow no extra
  assert(p->rcv_ann_right_edge==p->rcv_nxt+p->rcv_wnd); // explicit grant is advertised now
 }
 for(unsigned round=0;round<100;++round) {
  for(unsigned i=0;i<32;++i) {
   auto *p=flows[i];unsigned amount=p->rcv_wnd;
   receive(p,amount);
   assert(!p->rcv_wnd);unsigned before=acks;
   tcp_recved(p,amount);
   assert(p->rcv_wnd>=256 && acks>before); // zero window reopens without peer input
   assert(bridge::tcp_rx_native_policy_healthy());
  }
  unsigned total=0;
  for(auto *p:flows) {
   total+=TCP_WND_MAX(p);
   assert(p->rcv_wnd<=TCP_WND_MAX(p));
   assert((int32_t)(p->rcv_ann_right_edge-p->rcv_nxt)<=int32_t(p->rcv_wnd));
  }
  assert(total<=12288);
 }
 // Native FIN correction is sequence credit once, without application recved.
 auto *p=flows[0]; receive(p,1);immediate_fin(p);
 receive(p,1);deferred_fin(p);assert(bridge::tcp_rx_native_policy_healthy());
 // Actual tcp_free frees the generation before allocator reuse of that address.
 auto *old=flows[0];tcp_free(old);flows[0]=tcp_alloc(1);
 assert(flows[0]==old && flows[0]->rcv_wnd==256);
 flows[0]->state=ESTABLISHED;
 for(auto *q:flows) tcp_free(q);
 assert(frees==33 && bridge::tcp_rx_native_policy_healthy());
 // Reclaimed discretionary credit must reach a different zero-window flow
 // through the timer poll, with no application read or peer packet to wake it.
 for(unsigned i=0;i<32;++i) {
  flows[i]=tcp_alloc(1);assert(flows[i]);flows[i]->state=ESTABLISHED;
  receive(flows[i],256);
 }
 tcp_fasttmr();
 unsigned grown=0,donor=32;
 for(unsigned i=0;i<32;++i) {
  assert(flows[i]->rcv_wnd==0 || flows[i]->rcv_wnd==256);
  if(flows[i]->rcv_wnd) {++grown;donor=i;}
 }
 assert(grown==16 && donor<32); // cursor persists across connection generations
 tcp_free(flows[donor]);flows[donor]=nullptr;
 tcp_fasttmr();grown=0;
 for(auto *q:flows) if(q && q->rcv_wnd) ++grown;
 assert(grown==16);
 for(auto *q:flows) if(q) tcp_free(q);
 assert(frees==65 && bridge::tcp_rx_native_policy_healthy());
 // TIME_WAIT no longer accepts application sequence data. Its native PCB and
 // advertised fields survive, but must not pin discretionary receive credit.
 auto *terminal=tcp_alloc(1);assert(terminal);terminal->state=ESTABLISHED;
 receive(terminal,256);tcp_recved(terminal,256);
 auto *waiting=tcp_alloc(1);assert(waiting);waiting->state=ESTABLISHED;
 receive(waiting,256);tcp_recved(waiting,256);
 const unsigned native_window=terminal->rcv_wnd;
 terminal->state=TIME_WAIT;tcp_tw_pcbs=terminal;
 tcp_fasttmr();assert(TCP_WND_MAX(terminal)==0);
 assert(terminal->rcv_wnd==native_window); // native TW wire/SYN semantics untouched
 tcp_recved(terminal,1); // a late netconn read cannot debit a retired generation
 assert(bridge::tcp_rx_native_policy_healthy());
 tcp_tw_pcbs=nullptr;tcp_free(terminal);tcp_free(waiting);

#if BL616CL_TCP_RX_MEMORY_OWNER
 memory_test();
#endif
}
'''
            path=temp/'test.cpp';path.write_text(source)
            for scale,owner,fair in ((0,0,0),(1,0,0),(0,1,0),(1,1,0),(0,1,1),(1,1,1)):
                binary=temp/'test'
                subprocess.run(['g++','-std=c++17','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-DBL616CL_TCP_RX_FAIR_WINDOW=1','-DBL616CL_TCP_RX_ATOMIC_RESERVE=1',f'-DBL616CL_TCP_RX_MEMORY_OWNER={owner}',f'-DBL616CL_TCP_RX_PHYSICAL_FAIR={fair}',
                    '-DBL616CL_STA_TCP_OWNED_RX=1','-DBL616CL_AP_TCP_OWNED_RX=1',f'-DLWIP_WND_SCALE={scale}',
                    '-I'+str(temp),'-I'+str(ROOT/'UNOR4BL616CLBridge'),str(path),
                    str(ROOT/'UNOR4BL616CLBridge/tcp_rx_credit_pool.cpp'),
                    str(ROOT/'UNOR4BL616CLBridge/tcp_rx_native_policy.cpp'),'-o',str(binary)],check=True)
                subprocess.run([str(binary)],check=True)

if __name__=='__main__':unittest.main()
