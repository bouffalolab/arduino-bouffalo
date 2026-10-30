"""Exercise production RX chunk ownership using native pbuf copy/cat functions."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

def function(text, signature):
    start=text.index(signature); brace=text.index('{',start);end=brace+1;depth=1
    while depth:
        depth+=(text[end]=='{')-(text[end]=='}');end+=1
    return text[start:end]

class RxChunksTest(unittest.TestCase):
    def test_native_chain_copy_and_cleanup(self):
        with tempfile.TemporaryDirectory() as d:
            temp=Path(d); (temp/'lwip').mkdir();(temp/'sdk/mm').mkdir(parents=True)
            (temp/'sdk/mm/mm.h').write_text('#include <stdint.h>\nstatic inline uintptr_t mm_lock_save(void) { return 0; }\nstatic inline void mm_unlock_restore(uintptr_t f) { (void)f; }\n')
            (temp/'lwip/pbuf.h').write_text('''#include <stdint.h>
typedef uint16_t u16_t;typedef uint8_t u8_t;
struct pbuf { struct pbuf *next; void *payload; u16_t tot_len,len; unsigned ref; };
#define PBUF_RAW 0
#define PBUF_RAM 1
u16_t pbuf_copy_partial(const struct pbuf*,void*,u16_t,u16_t);
struct pbuf *pbuf_alloc(int,u16_t,int);
void pbuf_cat(struct pbuf*,struct pbuf*);
u8_t pbuf_free(struct pbuf*);
''')
            (temp/'lwip/opt.h').write_text('#define MEM_SIZE 24576U\n#define MEM_ALIGNMENT 4U\n')
            (temp/'lwip/stats.h').write_text('struct lwip_stats_stub { struct { unsigned used; } mem; };\nextern struct lwip_stats_stub lwip_stats;\n')
            native=(ROOT/'third_party/bouffalo_sdk/components/net/lwip/lwip/src/core/pbuf.c').read_text()
            source=r'''#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "sta_rx_ownership.h"
extern "C" {
#include "lwip/pbuf.h"
}
#define LWIP_ERROR(m,c,h) do { if(!(c)) {h;} } while(0)
#define LWIP_ASSERT(m,c) assert(c)
#define LWIP_DEBUGF(a,b)
#define LWIP_MIN(a,b) ((a)<(b)?(a):(b))
#define MEMCPY memcpy
'''
            source+='\nextern "C" u16_t\n'+function(native,'pbuf_copy_partial(const struct pbuf *buf, void *dataptr, u16_t len, u16_t offset)')
            source+='\nextern "C" void\n'+function(native,'pbuf_cat(struct pbuf *h, struct pbuf *t)')
            source+=r'''
static unsigned allocation_limit=1384;
static pbuf *radio;static unsigned live,allocations,radio_frees,fail_at,max_request,expected_denials;
#if BL616CL_TCP_RX_ATOMIC_RESERVE
static unsigned reserved_calls;
#endif
extern "C" pbuf *pbuf_alloc(int layer,u16_t length,int type) {
 assert(layer==PBUF_RAW && type==PBUF_RAM);++allocations;
 if(length>max_request)max_request=length;
 // Model the observed 1384-byte maximum allocator hole including pbuf/header charge.
 if(length+28U>allocation_limit || allocations==fail_at)return nullptr;
 pbuf *p=(pbuf *)malloc(sizeof(pbuf));assert(p);
 p->payload=malloc(length);assert(p->payload);p->len=p->tot_len=length;p->next=nullptr;p->ref=1;++live;return p;
}
#if BL616CL_TCP_RX_ATOMIC_RESERVE
extern "C" pbuf *bridge_pbuf_alloc_reserved(uint16_t length,unsigned reserve,int *denied) {
 ++reserved_calls; assert(reserve==4096); *denied=allocations+1==fail_at;
 if (*denied) ++expected_denials;
 return pbuf_alloc(PBUF_RAW,length,PBUF_RAM);
}
#endif
extern "C" u8_t pbuf_free(pbuf *p) {
 if(p==radio) { ++radio_frees;return 1; }
 unsigned n=0;
 while(p) { assert(p->ref==1 && live);--live;++n;pbuf *next=p->next;free(p->payload);free(p);p=next; }
 return n;
}
int main() {
 for(unsigned length: {54U,256U,512U,1024U,1025U,1514U}) {
  unsigned char bytes[1514],output[1514];for(unsigned i=0;i<length;++i)bytes[i]=(i*37U)%256;
  bytes[12]=8;bytes[13]=0;bytes[14]=0x4f;bytes[23]=6;
  // A source chain split inside Ethernet/IP headers, as well as the data region.
  pbuf tail={nullptr,bytes+17,(u16_t)(length-17),(u16_t)(length-17),1};
  pbuf head={&tail,bytes,(u16_t)length,17,1};radio=&head;
  allocations=radio_frees=max_request=0;fail_at=0;
  pbuf *copy=(pbuf *)bridge::sta_rx_detach(true,radio);
  assert(copy && copy->tot_len==length && copy->len<=BL616CL_STA_TCP_RX_CHUNK_BYTES);
  assert(radio_frees==1 && max_request<=BL616CL_STA_TCP_RX_CHUNK_BYTES);
  assert(pbuf_copy_partial(copy,output,length,0)==length && !memcmp(bytes,output,length));
  unsigned nodes=0;
  for(pbuf *q=copy;q;q=q->next) {assert(q->ref==1 && q->tot_len==length-nodes*BL616CL_STA_TCP_RX_CHUNK_BYTES);++nodes;}
  assert(nodes==(length+BL616CL_STA_TCP_RX_CHUNK_BYTES-1)/BL616CL_STA_TCP_RX_CHUNK_BYTES);
  pbuf_free(copy);assert(!live);
  // Fail each allocation position, freeing every owned part and the radio once.
  for(unsigned fail=1;fail<=nodes;++fail) {
   allocations=radio_frees=0;fail_at=fail;
   assert(!bridge::sta_rx_detach(true,radio));assert(!live && radio_frees==1);
  }
  // A truncated source must free the current part and all previously copied parts.
  fail_at=0;allocations=radio_frees=0;--tail.len;
  assert(!bridge::sta_rx_detach(true,radio));assert(!live && radio_frees==1);
 }
 // A 16 KiB UDP datagram must use twelve nodes, not 23 TCP chunks,
 // below IP_REASS_MAX_PBUFS=22. Preserve each chained source byte exactly.
 allocation_limit=1600;
 for(unsigned fragment=0;fragment<12;++fragment) {
  unsigned char data[1514]={},out[1514];
  for(unsigned i=0;i<1514;++i)data[i]=(i*17+fragment)%251;
  data[12]=8;data[13]=0;data[14]=0x45;data[23]=17;
  data[20]=fragment==0?0x20:0;data[21]=fragment;
  pbuf tail={nullptr,data+17,1497,1497,1};
  pbuf head={&tail,data,1514,17,1};radio=&head;
  allocations=radio_frees=0;fail_at=0;
  pbuf *copy=(pbuf*)bridge::sta_rx_detach(true,radio);
  assert(copy && !copy->next && copy->tot_len==1514 && allocations==1 && radio_frees==1);
  assert(pbuf_copy_partial(copy,out,1514,0)==1514 && !memcmp(data,out,1514));
  pbuf_free(copy);assert(!live);
  allocations=radio_frees=0;fail_at=1;
  assert(!bridge::sta_rx_detach(true,radio) && !live && radio_frees==1);
 }
#if BL616CL_TCP_RX_ATOMIC_RESERVE
 // Header-only ACK retains the normal allocator path, even with RX admission on.
 unsigned char ack[54]={};ack[12]=8;ack[14]=0x45;ack[17]=40;ack[23]=6;ack[46]=0x50;ack[47]=0x10;
 pbuf header={nullptr,ack,54,54,1};radio=&header;
 unsigned before=reserved_calls;fail_at=0;
 pbuf *control=(pbuf *)bridge::sta_rx_detach(true,radio);
 assert(control && reserved_calls==before);pbuf_free(control);assert(!live);
#endif
 uint8_t report[64]={};bridge::sta_rx_ownership_report(report);
 assert(report[61]==BL616CL_TCP_RX_ATOMIC_RESERVE);
 unsigned denials=report[28]|unsigned(report[29])<<8|unsigned(report[30])<<16|unsigned(report[31])<<24;
 assert(denials==expected_denials);
 unsigned chunk=report[36]|unsigned(report[37])<<8;assert(chunk==BL616CL_STA_TCP_RX_CHUNK_BYTES);
}
'''
            source='#include <initializer_list>\n'+source
            main=temp/'test.cpp';main.write_text(source)
            for chunk,atomic in ((256,0),(512,0),(1024,0),(256,1),(1024,1)):
                binary=temp/'test'
                subprocess.run(['g++','-std=c++11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-DBL616CL_STA_TCP_OWNED_RX=1',f'-DBL616CL_STA_TCP_RX_CHUNK_BYTES={chunk}',
                    f'-DBL616CL_TCP_RX_ATOMIC_RESERVE={atomic}',f'-DBL616CL_STA_TCP_RX_RESERVE_BYTES={4096 if atomic else 0}',
                    '-I'+str(temp),'-I'+str(ROOT/'UNOR4BL616CLBridge'),str(main),str(ROOT/'UNOR4BL616CLBridge/sta_rx_ownership.cpp'),'-o',str(binary)],check=True)
                subprocess.run([str(binary)],check=True)

if __name__=='__main__':unittest.main()
