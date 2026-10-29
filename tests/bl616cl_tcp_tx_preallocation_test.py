"""Execute the patched native TX allocator, preserving single-pbuf DMA headroom."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
PATCHES=ROOT/'tools/bridge_runtime/profiles/bridge/fhost_patches'

def function(text,signature):
    start=text.index(signature);brace=text.index('{',start);end=brace+1;depth=1
    while depth:depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

class TxPreallocationTest(unittest.TestCase):
    def test_native_preallocation(self):
        with tempfile.TemporaryDirectory() as d:
            temp=Path(d)
            for relative in ('core/tcp.c','core/tcp_in.c','core/tcp_out.c','include/lwip/tcp.h'):
                path=Path('components/net/lwip/lwip/src')/relative
                p=temp/path;p.parent.mkdir(parents=True,exist_ok=True);p.write_bytes((ROOT/'third_party/bouffalo_sdk'/path).read_bytes())
            for name in ('0070-bridge-tcp-listen-ingress-diagnostic.patch','0075-bridge-tcp-receive-window-diagnostic.patch','0077-bridge-tcp-tx-preallocation-diagnostic.patch'):
                subprocess.run(['patch','-s','-p1','-i',str(PATCHES/name)],cwd=temp,check=True)
            text=(temp/'components/net/lwip/lwip/src/core/tcp_out.c').read_text()
            native=function(text,'tcp_pbuf_prealloc(pbuf_layer layer, u16_t length, u16_t max_length,')
            source=r'''#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef uint16_t u16_t;typedef uint32_t u32_t;typedef uint8_t u8_t;typedef unsigned pbuf_layer;
#define TCP_OVERSIZE 1460U
#define TCP_OVERSIZE_CALC_LENGTH(n) ((n)+TCP_OVERSIZE)
#define LWIP_MEM_ALIGN_SIZE(n) (((n)+3U)&~3U)
#define LWIP_MIN(a,b) ((a)<(b)?(a):(b))
#define LWIP_ASSERT(m,c) assert(c)
#define LWIP_UNUSED_ARG(a) (void)(a)
#define TCP_WRITE_FLAG_MORE 2
#define TF_NODELAY 1
#define PBUF_RAM 1
struct pbuf { struct pbuf *next;u16_t len,tot_len; };
struct tcp_pcb {unsigned flags;void *unsent,*unacked;};
static struct pbuf buffer;
static unsigned charged,fail;
static struct pbuf *pbuf_alloc(pbuf_layer layer,u16_t length,unsigned type) {
 assert(layer==464 && type==PBUF_RAM);
 charged=16+layer+LWIP_MEM_ALIGN_SIZE(length);
 if(fail)return NULL;
 buffer.next=NULL;buffer.len=buffer.tot_len=length;return &buffer;
}
extern unsigned bridge_tcp_tx_extra_bytes(void) __attribute__((weak));
'''+ 'static struct pbuf *\n'+native+r'''
int main(void) {
 struct tcp_pcb pcb={0,NULL,(void*)1};u16_t extra=999;
 struct pbuf *p=tcp_pbuf_prealloc(464,1,1460,&extra,&pcb,TCP_WRITE_FLAG_MORE,1);
 unsigned expected=(TEST_CAP>=0 && TEST_CAP<=1460 && TEST_CAP<1459)?1+TEST_CAP:1460;
 assert(p && p->next==NULL && p->len==1 && p->tot_len==1 && extra==expected-1);
 assert(charged==480+LWIP_MEM_ALIGN_SIZE(expected));
 if(TEST_CAP==0) {assert(charged==484);assert(16*(charged+8)<24576);}
 if(TEST_CAP<0) {assert(charged==1940);assert(16*(charged+8)>24576);}
 for(unsigned length=1;length<=1460;length+=37) {
  for(unsigned flags=0;flags<2;++flags) {
   pcb.flags=flags;pcb.unacked=NULL;
   p=tcp_pbuf_prealloc(464,length,1460,&extra,&pcb,0,1);
   assert(p && p->len==length && p->tot_len==length && p->next==NULL);
   unsigned original=LWIP_NETIF_TX_SINGLE_PBUF?1460:length;
   unsigned want=original;
   if(TEST_CAP>=0 && TEST_CAP<=1460 && want>length+TEST_CAP)want=length+TEST_CAP;
   assert(extra==want-length && charged==480+LWIP_MEM_ALIGN_SIZE(want));
  }
 }
 // A wide length+extra sum cannot wrap and yield an undersized pbuf.
 pcb.flags=0;pcb.unacked=(void*)1;
 p=tcp_pbuf_prealloc(464,65530,65535,&extra,&pcb,TCP_WRITE_FLAG_MORE,1);
 assert(p && p->len==65530 && extra<=5);
 fail=1;extra=123;
 assert(!tcp_pbuf_prealloc(464,1000,1460,&extra,&pcb,0,1) && extra==123);
}
'''
            main=temp/'test.c';main.write_text(source)
            for cap in (-1,0,64,1460,1461):
                for single in (0,1):
                    args=['gcc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',f'-DTEST_CAP={cap}',f'-DLWIP_NETIF_TX_SINGLE_PBUF={single}',str(main)]
                    if cap>=0:
                        callback=temp/'callback.c';callback.write_text(f'unsigned bridge_tcp_tx_extra_bytes(void) {{ return {cap}; }}\n');args.append(str(callback))
                    binary=temp/'test';subprocess.run(args+['-o',str(binary)],check=True);subprocess.run([str(binary)],check=True)
    def test_default_callback_absent(self):
        with tempfile.TemporaryDirectory() as d:
            obj=Path(d)/'test.o'
            subprocess.run(['g++','-std=c++11','-c',str(ROOT/'UNOR4BL616CLBridge/tcp_tx_preallocation.cpp'),'-o',str(obj)],check=True)
            self.assertNotIn('bridge_tcp_tx_extra_bytes',subprocess.check_output(['nm',str(obj)],text=True))
if __name__=='__main__':unittest.main()
